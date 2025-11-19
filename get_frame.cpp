/*
 *  EOP-style single-frame extractor (máxima pureza Stepanov)
 *
 *  Filosofia EOP:
 *  1. Tipos regulares como valores matemáticos
 *  2. Funções livres > métodos
 *  3. Conceitos explícitos via requires/constraints
 *  4. Algoritmos genéricos puros
 *  5. Minimizar abstrações OO
 *  6. Transformações e predicados como funções de primeira classe
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <stdexcept>
#include <iterator>
#include <algorithm>
#include <memory>
#include <functional>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

/* ========================================================================
 *  PARTE 1: Tipos Regulares Primitivos
 *
 *  Regular type: tipo que se comporta como valor matemático
 *  - Cópia cria equivalente
 *  - Igualdade é reflexiva, simétrica, transitiva
 *  - Assignment preserva igualdade
 * ========================================================================
 */

// VideoFrame: valor imutável representando um frame
struct VideoFrame
{
    AVFrame* ptr;  // não-owning pointer (lifetime gerenciado
                   // externamente)

    // Construtor trivial
    VideoFrame() : ptr(nullptr)
    {
    }
    explicit VideoFrame(AVFrame* p) : ptr(p)
    {
    }

    // Regular type: trivialmente copiável (shallow copy)
    VideoFrame(const VideoFrame&)            = default;
    VideoFrame& operator=(const VideoFrame&) = default;

    // Igualdade baseada em identidade
    friend bool operator==(VideoFrame a, VideoFrame b)
    {
        return a.ptr == b.ptr;
    }

    friend bool operator!=(VideoFrame a, VideoFrame b)
    {
        return !(a == b);
    }
};

// Predicado: frame é válido?
inline bool is_valid(VideoFrame f)
{
    return f.ptr != nullptr;
}

// Acessores (funções livres)
inline int width(VideoFrame f)
{
    return f.ptr ? f.ptr->width : 0;
}

inline int height(VideoFrame f)
{
    return f.ptr ? f.ptr->height : 0;
}

inline AVPixelFormat pixel_format(VideoFrame f)
{
    return f.ptr ? static_cast<AVPixelFormat>(f.ptr->format)
                 : AV_PIX_FMT_NONE;
}

// Função de liberação (explícita, não automática)
inline void destroy(VideoFrame& f)
{
    if(f.ptr)
    {
        av_frame_free(&f.ptr);
        f.ptr = nullptr;
    }
}

// Função de clonagem (cópia profunda explícita)
inline VideoFrame clone(VideoFrame f)
{
    return f.ptr ? VideoFrame{av_frame_clone(f.ptr)} : VideoFrame{};
}

/* ========================================================================
 *  PARTE 2: VideoDecoder - Máquina de Estados Explícita
 *
 *  Em EOP, preferimos tipos que representam estados computacionais
 *  de forma explícita, sem encapsulamento excessivo.
 * ========================================================================
 */

struct VideoDecoder
{
    AVFormatContext* format_context;
    AVCodecContext*  codec_context;
    AVPacket*        packet;
    int              video_stream_index;
    bool             is_open;
    bool             is_exhausted;

    // Construtor trivial - estado "não inicializado"
    VideoDecoder()
        : format_context(nullptr), codec_context(nullptr),
          packet(nullptr), video_stream_index(-1), is_open(false),
          is_exhausted(false)
    {
    }
};

// Predicados sobre estados
inline bool is_open(const VideoDecoder& dec)
{
    return dec.is_open;
}

inline bool is_exhausted(const VideoDecoder& dec)
{
    return dec.is_exhausted;
}

// Transição de estado: não-inicializado → aberto
inline bool open(VideoDecoder& dec, const char* path)
{
    // Pré-condição: !is_open(dec)

    if(avformat_open_input(
           &dec.format_context, path, nullptr, nullptr) < 0)
        return false;

    if(avformat_find_stream_info(dec.format_context, nullptr) < 0)
        return false;

    // Encontra stream de vídeo
    for(unsigned i = 0; i < dec.format_context->nb_streams; ++i)
    {
        if(dec.format_context->streams[i]->codecpar->codec_type ==
           AVMEDIA_TYPE_VIDEO)
        {
            dec.video_stream_index = static_cast<int>(i);
            break;
        }
    }

    if(dec.video_stream_index == -1)
        return false;

    // Inicializa codec
    const AVCodec* codec = avcodec_find_decoder(
        dec.format_context->streams[dec.video_stream_index]
            ->codecpar->codec_id);

    if(!codec)
        return false;

    dec.codec_context = avcodec_alloc_context3(codec);
    if(!dec.codec_context)
        return false;

    if(avcodec_parameters_to_context(
           dec.codec_context,
           dec.format_context->streams[dec.video_stream_index]
               ->codecpar) < 0)
        return false;

    if(avcodec_open2(dec.codec_context, codec, nullptr) < 0)
        return false;

    dec.packet = av_packet_alloc();
    if(!dec.packet)
        return false;

    dec.is_open      = true;
    dec.is_exhausted = false;

    return true;
}

// Transição de estado: aberto → fechado
inline void close(VideoDecoder& dec)
{
    if(dec.packet)
    {
        av_packet_free(&dec.packet);
        dec.packet = nullptr;
    }
    if(dec.codec_context)
    {
        avcodec_free_context(&dec.codec_context);
        dec.codec_context = nullptr;
    }
    if(dec.format_context)
    {
        avformat_close_input(&dec.format_context);
        dec.format_context = nullptr;
    }
    dec.is_open      = false;
    dec.is_exhausted = false;
}

// Operação: lê próximo frame
// Retorna: VideoFrame válido ou inválido (EOF)
inline VideoFrame read_frame(VideoDecoder& dec)
{
    // Pré-condição: is_open(dec) && !is_exhausted(dec)

    if(!dec.is_open || dec.is_exhausted)
        return VideoFrame{};

    while(av_read_frame(dec.format_context, dec.packet) >= 0)
    {
        if(dec.packet->stream_index != dec.video_stream_index)
        {
            av_packet_unref(dec.packet);
            continue;
        }

        if(avcodec_send_packet(dec.codec_context, dec.packet) < 0)
        {
            av_packet_unref(dec.packet);
            continue;
        }
        av_packet_unref(dec.packet);

        AVFrame* raw = av_frame_alloc();
        if(!raw)
        {
            dec.is_exhausted = true;
            return VideoFrame{};
        }

        int ret = avcodec_receive_frame(dec.codec_context, raw);

        if(ret == AVERROR(EAGAIN))
        {
            av_frame_free(&raw);
            continue;
        }

        if(ret < 0)
        {
            av_frame_free(&raw);
            dec.is_exhausted = true;
            return VideoFrame{};
        }

        return VideoFrame{raw};
    }

    // Flush decoder
    avcodec_send_packet(dec.codec_context, nullptr);
    AVFrame* raw = av_frame_alloc();
    if(raw && avcodec_receive_frame(dec.codec_context, raw) == 0)
    {
        return VideoFrame{raw};
    }
    if(raw)
        av_frame_free(&raw);

    dec.is_exhausted = true;
    return VideoFrame{};
}

/* ========================================================================
 *  PARTE 3: Iterador como Ponteiro Generalizado
 *
 *  Iterator em EOP: generalização de ponteiros
 *  - Denota posição em sequência
 *  - successor(i) avança para próxima posição
 *  - source(i) retorna valor na posição
 * ========================================================================
 */

struct VideoFrameIterator
{
    VideoDecoder* decoder;  // não-owning
    VideoFrame    current_frame;
    bool          at_end;

    VideoFrameIterator()
        : decoder(nullptr), current_frame(), at_end(true)
    {
    }

    explicit VideoFrameIterator(VideoDecoder* dec)
        : decoder(dec), current_frame(), at_end(false)
    {
        advance();  // carrega primeiro frame
    }

    void advance()
    {
        if(at_end)
            return;

        // Libera frame anterior
        if(is_valid(current_frame))
        {
            destroy(current_frame);
        }

        current_frame = read_frame(*decoder);

        if(!is_valid(current_frame))
        {
            at_end = true;
        }
    }
};

// Traits para std::iterator_traits
namespace std
{
template <> struct iterator_traits<VideoFrameIterator>
{
    using iterator_category = input_iterator_tag;
    using value_type        = VideoFrame;
    using difference_type   = ptrdiff_t;
    using pointer           = const VideoFrame*;
    using reference         = const VideoFrame&;
};
}  // namespace std

// Operações de iterador (funções livres)

// source(i): valor apontado
inline VideoFrame source(const VideoFrameIterator& i)
{
    return i.current_frame;
}

// successor(i): próxima posição
inline void successor(VideoFrameIterator& i)
{
    i.advance();
}

// Igualdade: dois iteradores são iguais se ambos estão no fim
inline bool operator==(const VideoFrameIterator& a,
                       const VideoFrameIterator& b)
{
    return a.at_end == b.at_end;
}

inline bool operator!=(const VideoFrameIterator& a,
                       const VideoFrameIterator& b)
{
    return !(a == b);
}

// Interface C++ padrão (adaptadores)
inline VideoFrame operator*(const VideoFrameIterator& i)
{
    return source(i);
}

inline VideoFrameIterator& operator++(VideoFrameIterator& i)
{
    successor(i);
    return i;
}

inline VideoFrameIterator operator++(VideoFrameIterator& i, int)
{
    VideoFrameIterator tmp = i;
    successor(i);
    return tmp;
}

/* ========================================================================
 *  PARTE 4: Algoritmos Genéricos Puros
 *
 *  Algoritmos em EOP são funções matemáticas sobre conceitos
 * abstratos. Não dependem de tipos concretos, apenas de operações
 * garantidas.
 * ========================================================================
 */

// advance_n: avança iterador n posições
// Conceito: ForwardIterator
// Complexidade: O(n)
template <typename I>
I advance_n(I                                                 i,
            typename std::iterator_traits<I>::difference_type n)
{
    // Pré-condição: n >= 0 && i é válido para n avanços
    // Pós-condição: retorna iterador n posições à frente

    while(n > 0)
    {
        ++i;
        --n;
    }
    return i;
}

// nth_element_or_default: retorna n-ésimo elemento ou valor default
// Conceito: InputIterator
template <typename I>
typename std::iterator_traits<I>::value_type nth_element_or_default(
    I                                                 first,
    I                                                 last,
    typename std::iterator_traits<I>::difference_type n,
    typename std::iterator_traits<I>::value_type      default_value)
{
    // Invariante: distância percorrida <= n
    I                                                 i = first;
    typename std::iterator_traits<I>::difference_type k = 0;

    while(k < n && i != last)
    {
        ++i;
        ++k;
    }

    return (i != last) ? *i : default_value;
}

// nth_element: versão sem default (retorna valor na posição ou
// invalido)
template <typename I>
typename std::iterator_traits<I>::value_type
nth_element(I                                                 first,
            I                                                 last,
            typename std::iterator_traits<I>::difference_type n)
{
    using T = typename std::iterator_traits<I>::value_type;
    return nth_element_or_default(first, last, n, T{});
}

// for_each: aplica função em cada elemento
// Conceito: InputIterator, UnaryFunction
template <typename I, typename F> F for_each(I first, I last, F f)
{
    while(first != last)
    {
        f(*first);
        ++first;
    }
    return f;
}

// count_if: conta elementos que satisfazem predicado
// Conceito: InputIterator, UnaryPredicate
template <typename I, typename P>
typename std::iterator_traits<I>::difference_type
count_if(I first, I last, P pred)
{
    typename std::iterator_traits<I>::difference_type n = 0;
    while(first != last)
    {
        if(pred(*first))
            ++n;
        ++first;
    }
    return n;
}

/* ========================================================================
 *  PARTE 5: Transformações e I/O
 *
 *  Operações que não são algorítmicas, mas sim efeitos colaterais.
 * ========================================================================
 */

// Transformação: VideoFrame → RGB24
struct RGBFrame
{
    AVFrame* ptr;

    RGBFrame() : ptr(nullptr)
    {
    }
    explicit RGBFrame(AVFrame* p) : ptr(p)
    {
    }
};

inline void destroy(RGBFrame& f)
{
    if(f.ptr)
    {
        av_frame_free(&f.ptr);
        f.ptr = nullptr;
    }
}

inline RGBFrame to_rgb(VideoFrame src)
{
    if(!is_valid(src))
        return RGBFrame{};

    SwsContext* sws = sws_getContext(width(src),
                                     height(src),
                                     pixel_format(src),
                                     width(src),
                                     height(src),
                                     AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);

    if(!sws)
        return RGBFrame{};

    AVFrame* rgb = av_frame_alloc();
    if(!rgb)
    {
        sws_freeContext(sws);
        return RGBFrame{};
    }

    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = width(src);
    rgb->height = height(src);

    if(av_frame_get_buffer(rgb, 0) < 0)
    {
        av_frame_free(&rgb);
        sws_freeContext(sws);
        return RGBFrame{};
    }

    sws_scale(sws,
              src.ptr->data,
              src.ptr->linesize,
              0,
              height(src),
              rgb->data,
              rgb->linesize);

    sws_freeContext(sws);
    return RGBFrame{rgb};
}

// Efeito colateral: escreve frame em arquivo PPM
inline bool write_ppm(RGBFrame rgb, const char* path)
{
    if(!rgb.ptr)
        return false;

    FILE* f = std::fopen(path, "wb");
    if(!f)
        return false;

    std::fprintf(
        f, "P6\n%d %d\n255\n", rgb.ptr->width, rgb.ptr->height);

    for(int y = 0; y < rgb.ptr->height; ++y)
    {
        std::fwrite(rgb.ptr->data[0] + y * rgb.ptr->linesize[0],
                    1,
                    rgb.ptr->width * 3,
                    f);
    }

    std::fclose(f);
    return true;
}

/* ========================================================================
 *  PARTE 6: Composição (main = composição de transformações)
 * ========================================================================
 */

int main(int argc, char* argv[])
{
    if(argc != 4)
    {
        std::cerr << "uso: " << argv[0]
                  << " video.mp4 frame_index output.ppm\n";
        return EXIT_FAILURE;
    }

    av_log_set_level(AV_LOG_QUIET);

    const char* video_path  = argv[1];
    const long  frame_index = std::stol(argv[2]);
    const char* output_path = argv[3];

    // Inicializa decoder (máquina de estados)
    VideoDecoder decoder;

    if(!open(decoder, video_path))
    {
        std::cerr << "erro: não foi possível abrir vídeo\n";
        return EXIT_FAILURE;
    }

    // Cria range [begin, end)
    VideoFrameIterator first(&decoder);
    VideoFrameIterator last;

    // Algoritmo: extrai n-ésimo elemento
    VideoFrame target = nth_element(first, last, frame_index);

    if(!is_valid(target))
    {
        std::cerr << "erro: frame " << frame_index
                  << " não encontrado\n";
        close(decoder);
        return EXIT_FAILURE;
    }

    // Transformação: YUV → RGB
    RGBFrame rgb = to_rgb(target);

    // Efeito colateral: persiste em disco
    bool success = write_ppm(rgb, output_path);

    // Cleanup explícito (destruição manual de recursos)
    destroy(rgb);
    destroy(target);
    close(decoder);

    if(success)
    {
        std::cout << "frame " << frame_index << " salvo em "
                  << output_path << '\n';
        return EXIT_SUCCESS;
    }
    else
    {
        std::cerr << "erro ao salvar frame\n";
        return EXIT_FAILURE;
    }
}
