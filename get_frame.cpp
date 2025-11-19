/*
 *  EOP-style single-frame extractor (versão com iterators)
 *  g++ (ou cmake) + FFmpeg
 *  Uso: ./get_frame video.mp4 150 out.ppm
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <stdexcept>
#include <iterator>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

/* ---------- Wrapper RAII para AVFrame* ---------- */

struct Frame {
    AVFrame* ptr{nullptr};

    Frame() = default;
    explicit Frame(AVFrame* p) : ptr(p) {}

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Frame(Frame&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    ~Frame() { reset(); }

    AVFrame* get() const { return ptr; }

    AVFrame* release() noexcept {
        AVFrame* tmp = ptr;
        ptr = nullptr;
        return tmp;
    }

    void reset(AVFrame* p = nullptr) {
        if (ptr) {
            av_frame_free(&ptr);
        }
        ptr = p;
    }

    explicit operator bool() const noexcept { return ptr != nullptr; }
};

/* ---------- Conceito (informal) de FrameSource ----------
 *
 *  T satisfaz FrameSource se possuir:
 *    - bool   open()
 *    - AVFrame* read()   // cada chamada devolve novo AVFrame* (caller é dono)
 *    - void   close()
 *
 *  Vamos modelar isso com VideoFile.
 */

/* ---------- Modelo concreto: VideoFile ---------- */

class VideoFile {
public:
    explicit VideoFile(const std::string& path) : path_(path) {}

    // não copiável
    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;

    // movível (opcional, mas útil)
    VideoFile(VideoFile&& other) noexcept
        : path_(std::move(other.path_)),
          fmt_(other.fmt_),
          codec_ctx_(other.codec_ctx_),
          pkt_(other.pkt_),
          stream_index_(other.stream_index_)
    {
        other.fmt_ = nullptr;
        other.codec_ctx_ = nullptr;
        other.pkt_ = nullptr;
        other.stream_index_ = -1;
    }

    VideoFile& operator=(VideoFile&& other) noexcept {
        if (this != &other) {
            close();
            path_ = std::move(other.path_);
            fmt_  = other.fmt_;
            codec_ctx_ = other.codec_ctx_;
            pkt_  = other.pkt_;
            stream_index_ = other.stream_index_;

            other.fmt_ = nullptr;
            other.codec_ctx_ = nullptr;
            other.pkt_ = nullptr;
            other.stream_index_ = -1;
        }
        return *this;
    }

    bool open()
    {
        if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0)
            return false;

        if (avformat_find_stream_info(fmt_, nullptr) < 0)
            return false;

        // encontra o primeiro stream de vídeo
        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                stream_index_ = static_cast<int>(i);
                break;
            }
        }
        if (stream_index_ == -1) return false;

        const AVCodec* codec =
            avcodec_find_decoder(fmt_->streams[stream_index_]->codecpar->codec_id);
        if (!codec) return false;

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) return false;

        if (avcodec_parameters_to_context(
                codec_ctx_, fmt_->streams[stream_index_]->codecpar) < 0)
            return false;

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0)
            return false;

        pkt_ = av_packet_alloc();
        if (!pkt_) return false;

        return true;
    }

    // Lê próximo frame de vídeo.
    // Retorna AVFrame* alocado (caller deve liberar com av_frame_free).
    // Retorna nullptr em EOF ou erro.
    AVFrame* read()
    {
        while (av_read_frame(fmt_, pkt_) >= 0) {
            if (pkt_->stream_index != stream_index_) {
                av_packet_unref(pkt_);
                continue;
            }

            int ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);
            if (ret < 0) {
                // erro ao enviar o pacote, tenta próximo
                continue;
            }

            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                return nullptr;
            }

            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&frame);
                continue;
            }
            if (ret == AVERROR_EOF) {
                av_frame_free(&frame);
                return nullptr;
            }
            if (ret < 0) {
                av_frame_free(&frame);
                return nullptr;
            }

            // sucesso
            return frame;
        }

        return nullptr; // EOF
    }

    void close()
    {
        if (pkt_) {
            av_packet_free(&pkt_);
            pkt_ = nullptr;
        }
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
        if (fmt_) {
            avformat_close_input(&fmt_);
            fmt_ = nullptr;
        }
    }

    ~VideoFile() { close(); }

    /* ---------- Iteradores de frames ---------- */

    class FrameIterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = Frame;
        using difference_type   = std::ptrdiff_t;
        using pointer           = Frame*;
        using reference         = Frame&;

        FrameIterator() : src_(nullptr), current_() {}

        explicit FrameIterator(VideoFile* src) : src_(src), current_() {
            // carrega o primeiro frame
            ++(*this);
        }

        reference operator*()  { return current_; }
        pointer   operator->() { return &current_; }

        FrameIterator& operator++()
        {
            if (!src_) {
                // já é end
                return *this;
            }

            AVFrame* raw = src_->read();
            if (!raw) {
                // chegou no fim
                current_.reset();
                src_ = nullptr;
            } else {
                current_.reset(raw);
            }
            return *this;
        }

        FrameIterator operator++(int)
        {
            FrameIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(const FrameIterator& a, const FrameIterator& b)
        {
            // definimos "end" como src_ == nullptr
            return a.src_ == nullptr && b.src_ == nullptr;
        }

        friend bool operator!=(const FrameIterator& a, const FrameIterator& b)
        {
            return !(a == b);
        }

    private:
        VideoFile* src_;  // não é dono
        Frame      current_;
    };

    using iterator = FrameIterator;

    iterator begin() { return iterator(this); }
    iterator end()   { return iterator(); }   // iterador "end" default

private:
    std::string      path_;
    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_ctx_{nullptr};
    AVPacket*        pkt_{nullptr};
    int              stream_index_{-1};
};

/* ---------- Algoritmo genérico get_nth sobre iteradores ---------- */

template <typename I>
typename std::iterator_traits<I>::value_type
get_nth(I first, I last, std::size_t n)
{
    using value_type = typename std::iterator_traits<I>::value_type;

    for (std::size_t i = 0; i < n && first != last; ++i, ++first)
        ;

    if (first == last) {
        return value_type{}; // Frame default (ptr == nullptr)
    }

    // move o valor atual para o chamador
    return std::move(*first);
}

/* ---------- Salva frame como PPM ---------- */

void save_ppm(const AVFrame* fr, const std::string& out)
{
    if (!fr) return;

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open output");

    // Converte para RGB24
    SwsContext* sws = sws_getContext(
        fr->width, fr->height, static_cast<AVPixelFormat>(fr->format),
        fr->width, fr->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws) {
        std::fclose(f);
        throw std::runtime_error("cannot create sws context");
    }

    AVFrame* rgb = av_frame_alloc();
    if (!rgb) {
        sws_freeContext(sws);
        std::fclose(f);
        throw std::runtime_error("cannot allocate rgb frame");
    }

    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = fr->width;
    rgb->height = fr->height;

    if (av_frame_get_buffer(rgb, 0) < 0) {
        av_frame_free(&rgb);
        sws_freeContext(sws);
        std::fclose(f);
        throw std::runtime_error("cannot allocate rgb buffer");
    }

    sws_scale(sws, fr->data, fr->linesize, 0, fr->height,
              rgb->data, rgb->linesize);
    sws_freeContext(sws);

    std::fprintf(f, "P6\n%d %d\n255\n", fr->width, fr->height);
    for (int y = 0; y < fr->height; ++y) {
        std::fwrite(rgb->data[0] + y * rgb->linesize[0],
                    1, fr->width * 3, f);
    }

    std::fclose(f);
    av_frame_free(&rgb);
}

/* ---------- main ---------- */

int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::cerr << "uso: " << argv[0]
                  << " video.mp4 numero_frame out.ppm\n";
        return EXIT_FAILURE;
    }

    av_log_set_level(AV_LOG_QUIET);   // menos barulho

    const std::string path    = argv[1];
    const std::size_t nth     = std::stoul(argv[2]);
    const std::string out_ppm = argv[3];

    VideoFile vf(path);
    if (!vf.open()) {
        std::cerr << "não foi possível abrir o vídeo\n";
        return EXIT_FAILURE;
    }

    auto first = vf.begin();
    auto last  = vf.end();

    Frame frame = get_nth(first, last, nth);
    if (!frame) {
        std::cerr << "frame não encontrado\n";
        vf.close();
        return EXIT_FAILURE;
    }

    try {
        save_ppm(frame.get(), out_ppm);
        std::cout << "frame salvo em " << out_ppm << '\n';
    } catch (const std::exception& e) {
        std::cerr << "erro ao salvar frame: " << e.what() << '\n';
        vf.close();
        return EXIT_FAILURE;
    }

    vf.close();
    return EXIT_SUCCESS;
}

