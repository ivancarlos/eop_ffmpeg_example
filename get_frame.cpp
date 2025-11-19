/*
 *  EOP-style single-frame extractor (seguindo princípios de Stepanov)
 *  
 *  Princípios aplicados:
 *  1. Regular types (valores semânticos completos)
 *  2. Algoritmos genéricos sobre conceitos abstratos
 *  3. Separação entre estrutura de dados e algoritmos
 *  4. Iteradores como abstração de sequências
 *  5. Invariantes claros e pré/pós-condições
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <stdexcept>
#include <iterator>
#include <algorithm>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

/* ========================================================================
 *  PARTE 1: Tipos Regulares (Regular Types)
 *  
 *  Um tipo Regular em EOP possui:
 *  - Construtor default
 *  - Copy constructor e copy assignment
 *  - Destructor
 *  - Operadores de igualdade (==, !=)
 *  - Total ordering (<, >, <=, >=) quando aplicável
 * ======================================================================== */

// Deleter customizado para AVFrame (policy-based design)
struct AVFrameDeleter {
    void operator()(AVFrame* p) const {
        if (p) av_frame_free(&p);
    }
};

// Frame como tipo Regular (valor semântico)
class Frame {
    std::unique_ptr<AVFrame, AVFrameDeleter> frame_;

public:
    // Construtor default - estado vazio válido
    Frame() : frame_(nullptr) {}
    
    // Construtor a partir de ponteiro raw
    explicit Frame(AVFrame* p) : frame_(p) {}
    
    // Tipo Regular: copiável através de semântica de valor
    Frame(const Frame& other) : frame_(nullptr) {
        if (other.frame_) {
            frame_.reset(av_frame_clone(other.frame_.get()));
        }
    }
    
    Frame& operator=(const Frame& other) {
        if (this != &other) {
            if (other.frame_) {
                frame_.reset(av_frame_clone(other.frame_.get()));
            } else {
                frame_.reset();
            }
        }
        return *this;
    }
    
    // Movível (otimização)
    Frame(Frame&&) noexcept = default;
    Frame& operator=(Frame&&) noexcept = default;
    
    ~Frame() = default;
    
    // Operadores de comparação (baseados em identidade do ponteiro)
    friend bool operator==(const Frame& a, const Frame& b) {
        return a.frame_.get() == b.frame_.get();
    }
    
    friend bool operator!=(const Frame& a, const Frame& b) {
        return !(a == b);
    }
    
    // Acesso ao recurso subjacente
    AVFrame* get() const { return frame_.get(); }
    AVFrame* release() { return frame_.release(); }
    void reset(AVFrame* p = nullptr) { frame_.reset(p); }
    
    // Predicado: frame válido?
    explicit operator bool() const { return frame_ != nullptr; }
    
    // Propriedades do frame
    int width() const { return frame_ ? frame_->width : 0; }
    int height() const { return frame_ ? frame_->height : 0; }
    AVPixelFormat format() const { 
        return frame_ ? static_cast<AVPixelFormat>(frame_->format) : AV_PIX_FMT_NONE; 
    }
};

/* ========================================================================
 *  PARTE 2: Conceitos e Axiomas
 *  
 *  Readable: I i; => source(i) retorna o valor apontado
 *  Iterator: pode avançar (successor)
 *  InputIterator: Readable + Iterator + pode ser comparado com end
 * ======================================================================== */

// Conceito: FrameSource
// Requer: 
//   - bool open()
//   - Frame read()  (retorna próximo frame ou Frame vazio em EOF)
//   - void close()
// Invariante: read() só pode ser chamado após open() retornar true

/* ========================================================================
 *  PARTE 3: VideoFrameRange - Abstração de Sequência
 *  
 *  Range = par de iteradores [begin, end)
 * ======================================================================== */

class VideoFrameRange {
    // Estado interno
    struct VideoState {
        AVFormatContext* fmt_ctx{nullptr};
        AVCodecContext*  codec_ctx{nullptr};
        AVPacket*        packet{nullptr};
        int              stream_idx{-1};
        bool             exhausted{false};
        
        ~VideoState() {
            if (packet) av_packet_free(&packet);
            if (codec_ctx) avcodec_free_context(&codec_ctx);
            if (fmt_ctx) avformat_close_input(&fmt_ctx);
        }
    };
    
    std::shared_ptr<VideoState> state_;
    
    // Função auxiliar: lê próximo frame
    static Frame read_next_frame(VideoState* st) {
        if (!st || st->exhausted) return Frame{};
        
        while (av_read_frame(st->fmt_ctx, st->packet) >= 0) {
            if (st->packet->stream_index != st->stream_idx) {
                av_packet_unref(st->packet);
                continue;
            }
            
            if (avcodec_send_packet(st->codec_ctx, st->packet) < 0) {
                av_packet_unref(st->packet);
                continue;
            }
            av_packet_unref(st->packet);
            
            AVFrame* raw_frame = av_frame_alloc();
            if (!raw_frame) {
                st->exhausted = true;
                return Frame{};
            }
            
            int ret = avcodec_receive_frame(st->codec_ctx, raw_frame);
            if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&raw_frame);
                continue;
            }
            if (ret < 0) {
                av_frame_free(&raw_frame);
                st->exhausted = true;
                return Frame{};
            }
            
            return Frame{raw_frame};
        }
        
        // Flush do decoder
        avcodec_send_packet(st->codec_ctx, nullptr);
        AVFrame* raw_frame = av_frame_alloc();
        if (raw_frame && avcodec_receive_frame(st->codec_ctx, raw_frame) == 0) {
            return Frame{raw_frame};
        }
        if (raw_frame) av_frame_free(&raw_frame);
        
        st->exhausted = true;
        return Frame{};
    }

public:
    // InputIterator seguindo conceitos EOP
    class iterator {
        std::shared_ptr<VideoState> state_;
        Frame current_;
        bool is_end_;
        
        // Avança para próximo frame (successor function)
        void advance() {
            current_ = read_next_frame(state_.get());
            if (!current_) {
                is_end_ = true;
            }
        }
        
    public:
        // Traits obrigatórios
        using iterator_category = std::input_iterator_tag;
        using value_type        = Frame;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Frame*;
        using reference         = const Frame&;
        
        // Construtor de "end iterator"
        iterator() : state_(nullptr), current_(), is_end_(true) {}
        
        // Construtor de "begin iterator"
        explicit iterator(std::shared_ptr<VideoState> st) 
            : state_(st), current_(), is_end_(false) 
        {
            advance();  // carrega primeiro frame
        }
        
        // Readable concept: source(i)
        reference operator*() const { return current_; }
        pointer operator->() const { return &current_; }
        
        // Iterator concept: successor(i)
        iterator& operator++() {
            if (!is_end_) advance();
            return *this;
        }
        
        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }
        
        // Equality concept
        friend bool operator==(const iterator& a, const iterator& b) {
            // Dois iterators são iguais se ambos são "end"
            return a.is_end_ == b.is_end_;
        }
        
        friend bool operator!=(const iterator& a, const iterator& b) {
            return !(a == b);
        }
    };
    
    using const_iterator = iterator;
    
    // Construtor: abre vídeo e prepara decoder
    explicit VideoFrameRange(const std::string& path) 
        : state_(std::make_shared<VideoState>()) 
    {
        if (avformat_open_input(&state_->fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
            throw std::runtime_error("cannot open video file");
        }
        
        if (avformat_find_stream_info(state_->fmt_ctx, nullptr) < 0) {
            throw std::runtime_error("cannot find stream info");
        }
        
        // Localiza primeiro stream de vídeo
        for (unsigned i = 0; i < state_->fmt_ctx->nb_streams; ++i) {
            if (state_->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                state_->stream_idx = static_cast<int>(i);
                break;
            }
        }
        
        if (state_->stream_idx == -1) {
            throw std::runtime_error("no video stream found");
        }
        
        // Inicializa decoder
        const AVCodec* codec = avcodec_find_decoder(
            state_->fmt_ctx->streams[state_->stream_idx]->codecpar->codec_id
        );
        
        if (!codec) {
            throw std::runtime_error("codec not found");
        }
        
        state_->codec_ctx = avcodec_alloc_context3(codec);
        if (!state_->codec_ctx) {
            throw std::runtime_error("cannot allocate codec context");
        }
        
        if (avcodec_parameters_to_context(
                state_->codec_ctx,
                state_->fmt_ctx->streams[state_->stream_idx]->codecpar) < 0) {
            throw std::runtime_error("cannot copy codec parameters");
        }
        
        if (avcodec_open2(state_->codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("cannot open codec");
        }
        
        state_->packet = av_packet_alloc();
        if (!state_->packet) {
            throw std::runtime_error("cannot allocate packet");
        }
    }
    
    // Range interface
    iterator begin() { return iterator(state_); }
    iterator end() { return iterator(); }
    
    const_iterator begin() const { return iterator(state_); }
    const_iterator end() const { return iterator(); }
};

/* ========================================================================
 *  PARTE 4: Algoritmos Genéricos
 *  
 *  Em EOP, algoritmos operam sobre conceitos abstratos, não tipos concretos
 * ======================================================================== */

// nth_value: retorna o n-ésimo elemento de uma sequência
// Requer: InputIterator
// Complexidade: O(n) em tempo, O(1) em espaço
template <typename I>
typename std::iterator_traits<I>::value_type
nth_value(I first, I last, typename std::iterator_traits<I>::difference_type n)
{
    // Pré-condição: n >= 0
    // Pós-condição: retorna o n-ésimo elemento ou value_type{} se fora dos limites
    
    using value_type = typename std::iterator_traits<I>::value_type;
    
    // Avança n posições
    for (typename std::iterator_traits<I>::difference_type i = 0; 
         i < n && first != last; 
         ++i, ++first) {
        // invariante: i <= n && distância(original_first, first) == i
    }
    
    if (first == last) {
        return value_type{};  // estado vazio válido
    }
    
    return *first;
}

// distance: conta elementos entre dois iteradores
// (já existe em std::distance, mas ilustramos o conceito)
template <typename I>
typename std::iterator_traits<I>::difference_type
count_range(I first, I last)
{
    typename std::iterator_traits<I>::difference_type n = 0;
    while (first != last) {
        ++n;
        ++first;
    }
    return n;
}

/* ========================================================================
 *  PARTE 5: Operações de I/O (fora do núcleo algorítmico)
 * ======================================================================== */

void write_ppm(const Frame& frame, const std::string& path)
{
    if (!frame) {
        throw std::invalid_argument("cannot write empty frame");
    }
    
    AVFrame* src = frame.get();
    
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        throw std::runtime_error("cannot open output file");
    }
    
    // Conversão para RGB24
    SwsContext* sws = sws_getContext(
        src->width, src->height, static_cast<AVPixelFormat>(src->format),
        src->width, src->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws) {
        std::fclose(file);
        throw std::runtime_error("cannot create sws context");
    }
    
    AVFrame* rgb = av_frame_alloc();
    if (!rgb) {
        sws_freeContext(sws);
        std::fclose(file);
        throw std::runtime_error("cannot allocate rgb frame");
    }
    
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = src->width;
    rgb->height = src->height;
    
    if (av_frame_get_buffer(rgb, 0) < 0) {
        av_frame_free(&rgb);
        sws_freeContext(sws);
        std::fclose(file);
        throw std::runtime_error("cannot allocate rgb buffer");
    }
    
    sws_scale(sws, src->data, src->linesize, 0, src->height,
              rgb->data, rgb->linesize);
    
    // Escreve cabeçalho PPM
    std::fprintf(file, "P6\n%d %d\n255\n", src->width, src->height);
    
    // Escreve dados
    for (int y = 0; y < src->height; ++y) {
        std::fwrite(rgb->data[0] + y * rgb->linesize[0], 1, src->width * 3, file);
    }
    
    std::fclose(file);
    av_frame_free(&rgb);
    sws_freeContext(sws);
}

/* ========================================================================
 *  PARTE 6: Aplicação (composição de componentes genéricos)
 * ======================================================================== */

int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::cerr << "uso: " << argv[0] << " video.mp4 frame_number output.ppm\n";
        return EXIT_FAILURE;
    }
    
    av_log_set_level(AV_LOG_QUIET);
    
    try {
        const std::string video_path = argv[1];
        const std::size_t frame_num  = std::stoul(argv[2]);
        const std::string output_path = argv[3];
        
        // Cria range sobre frames do vídeo
        VideoFrameRange video(video_path);
        
        // Aplica algoritmo genérico para obter n-ésimo frame
        Frame target_frame = nth_value(video.begin(), video.end(), frame_num);
        
        if (!target_frame) {
            std::cerr << "frame " << frame_num << " não encontrado\n";
            return EXIT_FAILURE;
        }
        
        // Persiste resultado
        write_ppm(target_frame, output_path);
        
        std::cout << "frame " << frame_num << " salvo em " << output_path << '\n';
        
        return EXIT_SUCCESS;
        
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
