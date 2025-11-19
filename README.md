Dá para ficar **muito mais EOP-style**! O código atual ainda tem várias abstrações "orientadas a objeto" que Stepanov evitaria. Vou refatorar seguindo mais rigorosamente os princípios dele:## Agora sim está **muito mais EOP**! Principais mudanças:

### 1. **Funções Livres > Métodos**
- Tudo virou função livre: `is_valid()`, `width()`, `height()`, `destroy()`
- Sem encapsulamento OO desnecessário
- Funções operam sobre tipos como valores

### 2. **Tipos como Valores Matemáticos**
- `VideoFrame` é trivialmente copiável (shallow copy)
- `VideoDecoder` expõe seu estado explicitamente
- Sem RAII automático - destruição **explícita** com `destroy()`

### 3. **Máquina de Estados Explícita**
- `VideoDecoder` não esconde nada
- Transições: `open()`, `close()`, `read_frame()`
- Predicados: `is_open()`, `is_exhausted()`

### 4. **Iterator como "Ponteiro Generalizado"**
- Operações fundamentais: `source()`, `successor()`
- Adaptadores para C++ padrão (`operator*`, `operator++`)
- Semântica clara de posição em sequência

### 5. **Algoritmos Completamente Genéricos**
```cpp
template <typename I>
I advance_n(I i, difference_type n)  // funciona com QUALQUER iterador

template <typename I, typename P>
difference_type count_if(I first, I last, P pred)  // puro
```

### 6. **Separação Clara**
- **Valores**: `VideoFrame`, `RGBFrame`
- **Máquinas de estado**: `VideoDecoder`
- **Algoritmos**: `nth_element`, `for_each`, `count_if`
- **Transformações**: `to_rgb()`
- **I/O**: `write_ppm()`

### 7. **Lifetime Explícito**
```cpp
destroy(rgb);      // manual, não automático
destroy(target);   // programador controla
close(decoder);    // explícito
```

### 8. **Predicados e Transformações de Primeira Classe**
- `is_valid()` é predicado puro
- `to_rgb()` é transformação pura
- Podem ser passados como argumentos

---

**Dá para ir mais além?** Sim! Poderia:
1. Implementar conceitos C++20 explícitos (`requires`)
2. Adicionar mais álgebra (semigrupos, monoids)
3. Fazer `VideoDecoder` ser um **corrotina** (generator puro)
4. Adicionar tipos numéricos customizados (EOP cap. 2-3)

Mas isso aí já está **bem fiel ao espírito de Stepanov**: funções livres, tipos regulares, algoritmos genéricos, lifetime explícito! 🎯

## Lógica de Uso do FFmpeg (Fluxo Conceitual)

### **1. ABERTURA DO ARQUIVO (Format Layer)**

**avformat_open_input**: Abre o arquivo de vídeo e lê os primeiros bytes para identificar o formato (MP4, AVI, MKV, etc.). É como "abrir um livro" - você ainda não sabe o que tem dentro, só que conseguiu abrir.

**avformat_find_stream_info**: Analisa o arquivo para descobrir **o que ele contém**:
- Quantos streams existem? (vídeo, áudio, legendas...)
- Quais são as características de cada stream?
- Qual codec foi usado para codificar cada stream?

Depois disso você sabe: "esse arquivo tem 1 stream de vídeo H.264, 2 streams de áudio AAC, 1 stream de legenda".

---

### **2. SELEÇÃO DO STREAM DE VÍDEO**

Você percorre a lista de streams procurando pelo primeiro que seja do tipo **AVMEDIA_TYPE_VIDEO**.

É como folhear o índice de um livro procurando o capítulo que te interessa.

---

### **3. CONFIGURAÇÃO DO DECODER (Codec Layer)**

**avcodec_find_decoder**: Você descobriu que o vídeo está em H.264 (ou VP9, ou HEVC...). Agora precisa encontrar o **decodificador** apropriado para esse codec.

**avcodec_alloc_context3**: Cria um "contexto de decodificação" - é como preparar uma máquina específica para ler aquele tipo de dado codificado.

**avcodec_parameters_to_context**: Copia os parâmetros do stream (resolução, taxa de bits, formato de pixel) para o contexto do decoder.

**avcodec_open2**: "Liga" o decoder. Agora ele está pronto para receber dados comprimidos e devolver frames decodificados.

---

### **4. ALOCAÇÃO DE ESTRUTURAS**

**av_packet_alloc**: Cria um "pacote" que vai armazenar **dados comprimidos** lidos do arquivo. Um pacote pode conter:
- Um pedaço de um frame
- Um frame inteiro
- Múltiplos frames pequenos

É como uma caixa de correio onde chegam dados brutos ainda "embalados".

**av_frame_alloc**: Cria uma estrutura para armazenar um **frame decodificado** (imagem descomprimida). É onde o resultado final vai aparecer.

---

### **5. LOOP DE LEITURA E DECODIFICAÇÃO**

#### **5.1. Ler Pacote do Arquivo**
**av_read_frame**: Lê o próximo pacote do arquivo. Pode ser um pacote de vídeo, áudio ou legenda. Você precisa verificar o `stream_index` para saber se é do stream que te interessa.

#### **5.2. Enviar Pacote ao Decoder**
**avcodec_send_packet**: Envia o pacote comprimido para o decoder. É como colocar uma carta na máquina de decodificação.

**av_packet_unref**: Libera a memória do pacote (você já enviou, não precisa mais dele).

#### **5.3. Receber Frame Decodificado**
**avcodec_receive_frame**: Tenta pegar um frame decodificado do decoder. 

Pode retornar:
- **Sucesso**: frame pronto!
- **AVERROR(EAGAIN)**: decoder precisa de mais pacotes antes de produzir um frame
- **AVERROR_EOF**: acabaram os frames

**Por que EAGAIN?** Porque codecs usam **compressão inter-frame** (frames dependem de outros frames). Às vezes você precisa enviar 3-4 pacotes antes de conseguir 1 frame completo.

---

### **6. CONVERSÃO DE FORMATO (Scale/Convert)**

Os frames decodificados vêm em formatos específicos do codec (YUV420P, YUV422P, NV12...). Para salvar como imagem RGB (tipo PPM), você precisa **converter**.

**sws_getContext**: Cria um contexto de conversão especificando:
- Formato de origem (ex: YUV420P)
- Formato de destino (RGB24)
- Algoritmo de interpolação (bilinear, bicubic...)

**av_frame_get_buffer**: Aloca memória para o frame RGB de destino.

**sws_scale**: Executa a conversão. É como passar a imagem por um filtro que muda o esquema de cores.

---

### **7. FLUSH DO DECODER (Final)**

Quando `av_read_frame` retorna EOF (fim do arquivo), ainda podem existir frames "presos" dentro do decoder (buffered frames).

**avcodec_send_packet(nullptr)**: Sinaliza "acabou a entrada".

**avcodec_receive_frame** (em loop): Drena todos os frames restantes.

---

### **8. LIMPEZA**

Liberar tudo na ordem inversa:
1. **av_packet_free**
2. **av_frame_free** 
3. **avcodec_free_context** (fecha o decoder)
4. **avformat_close_input** (fecha o arquivo)
5. **sws_freeContext** (se usou conversão)

---

## **FLUXO RESUMIDO**

```
ARQUIVO (comprimido)
    ↓
[avformat] → lê pacotes comprimidos
    ↓
PACOTES (H.264/VP9/etc bytes)
    ↓
[avcodec] → decodifica
    ↓
FRAMES (YUV pixels)
    ↓
[swscale] → converte formato
    ↓
FRAMES RGB (pronto para salvar)
```

---

## **ANALOGIA**

- **Arquivo**: livro lacrado em idioma codificado
- **avformat**: abre o livro e identifica o idioma
- **avcodec**: tradutor especializado naquele idioma
- **Pacotes**: frases ainda codificadas
- **Frames**: frases traduzidas e legíveis
- **swscale**: mudança de fonte/formato de apresentação

