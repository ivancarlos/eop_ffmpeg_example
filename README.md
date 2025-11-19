Sim, conheço bem a técnica EOP (Elements of Programming)! Alexander Stepanov, junto com Paul McJones, formalizou esses princípios no livro "Elements of Programming" (2009), que estabelece fundamentos matemáticos rigorosos para programação genérica.

Vou refatorar seu código seguindo mais fielmente os princípios EOP:## Principais diferenças seguindo princípios EOP:

### 1. **Tipos Regulares (Regular Types)**
- `Frame` agora é totalmente copiável com semântica de valor
- Implementa todos os operadores necessários (==, !=)
- Estado vazio válido e bem definido

### 2. **Separação de Conceitos**
- **Range**: `VideoFrameRange` encapsula a sequência
- **Iterator**: implementação limpa separada do container
- **Algoritmos**: `nth_value` opera sobre conceitos abstratos

### 3. **Invariantes Claros**
- Cada função documenta pré/pós-condições
- Estados válidos explícitos
- Loops com invariantes comentados

### 4. **Composição ao invés de Hierarquia**
- Usa `shared_ptr` para compartilhar estado entre iteradores
- Policy-based design (`AVFrameDeleter`)
- Sem herança desnecessária

### 5. **Algoritmos Genéricos**
- `nth_value` funciona com qualquer `InputIterator`
- Não depende de tipos concretos
- Complexidade documentada

### 6. **Abstração de Sequências**
- Range como par de iteradores [begin, end)
- Interface limpa e minimalista
- Iterator "end" bem definido

Essa abordagem reflete a filosofia de Stepanov: **componentes genéricos, matematicamente fundamentados, que podem ser compostos livremente**.
