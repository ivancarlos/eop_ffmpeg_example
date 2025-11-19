Boa, isso é exatamente o tipo de coisa que o Stepanov aprovaria: deixar as capacidades do tipo **explícitas**.

Aqui está a mesma versão anterior, só que com `VideoFile` explicitamente **não copiável** (e movível, se você quiser passar para funções ou guardar em containers).

### Mudança principal: marcar como não copiável

```cpp
class VideoFile {
public:
    explicit VideoFile(const std::string& path) : path_(path) {}

    // --- proíbe cópia ---
    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;

    // --- permite movimento (opcional, mas útil) ---
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

    VideoFile& operator=(VideoFile&& other) noexcept
    {
        if (this != &other) {
            close(); // libera recursos atuais

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

            return frame;
        }

        return nullptr;
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

private:
    std::string      path_;
    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_ctx_{nullptr};
    AVPacket*        pkt_{nullptr};
    int              stream_index_{-1};
};
```

Nada no resto do código precisa mudar: o `main` continua igual, `get_nth_frame` continua recebendo `Src&`, etc.

Se tentar fazer:

```cpp
VideoFile a("a.mp4");
VideoFile b = a;          // ERRO: cópia deletada
VideoFile c(a);           // ERRO
```

o compilador reclama, como queremos.

Se quiser, no próximo passo a gente pode transformar isso em algo ainda mais “Stepanov”, com um `FrameIterator` para você fazer coisas como `get_nth(first, n)` mesmo.

