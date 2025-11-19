/*
 *  EOP-style single-frame extractor
 *  g++ (ou cmake) + FFmpeg
 *  Uso: ./get_frame video.mp4 150 out.ppm
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <stdexcept>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

/* ---------- Conceitos (EOP) ---------- */

// T satisfaz FrameSource se possuir:
//   - open()  -> bool
//   - read()  -> AVFrame*
//   - close() -> void
// (definido informalmente aqui)

/* ---------- Abstração genérica ---------- */

template <typename Src>
AVFrame* get_nth_frame(Src& src, std::size_t n)
{
    if (!src.open()) throw std::runtime_error("cannot open source");

    AVFrame* fr = nullptr;
    for (std::size_t i = 0; i <= n; ++i) {
        fr = src.read();          // pode retornar nullptr (EOF)
        if (!fr) break;
    }
    src.close();
    return fr;
}

/* ---------- Modelo concreto que satisfaz FrameSource ---------- */

class VideoFile {
public:
    explicit VideoFile(const std::string& path) : path_(path) {}

    bool open()
    {
        if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt_, nullptr) < 0)
            return false;

        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                stream_index_ = static_cast<int>(i);
                break;
            }
        if (stream_index_ == -1) return false;

        const AVCodec* codec = avcodec_find_decoder(
            fmt_->streams[stream_index_]->codecpar->codec_id);
        if (!codec) return false;

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) return false;
        avcodec_parameters_to_context(
            codec_ctx_, fmt_->streams[stream_index_]->codecpar);
        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) return false;

        frame_ = av_frame_alloc();
        pkt_   = av_packet_alloc();
        return true;
    }

    AVFrame* read()   // retorna nullptr em EOF ou erro
    {
        while (av_read_frame(fmt_, pkt_) >= 0) {
            if (pkt_->stream_index != stream_index_) {
                av_packet_unref(pkt_);
                continue;
            }
            int ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);
            if (ret < 0) continue;

            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                continue;
            if (ret < 0) return nullptr;
            return frame_;   // devolve ponteiro "vivo" (não copia)
        }
        return nullptr;
    }

    void close()
    {
        if (pkt_)   av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (codec_ctx_) avcodec_free_context(&codec_ctx_);
        if (fmt_)   avformat_close_input(&fmt_);
    }

    ~VideoFile() { close(); }

private:
    std::string path_;
    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_ctx_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* pkt_{nullptr};
    int stream_index_{-1};
};

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

    AVFrame* rgb = av_frame_alloc();
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = fr->width;
    rgb->height = fr->height;
    av_frame_get_buffer(rgb, 0);

    sws_scale(sws, fr->data, fr->linesize, 0, fr->height,
              rgb->data, rgb->linesize);
    sws_freeContext(sws);

    fprintf(f, "P6\n%d %d\n255\n", fr->width, fr->height);
    for (int y = 0; y < fr->height; ++y)
        std::fwrite(rgb->data[0] + y * rgb->linesize[0], 1, fr->width * 3, f);

    std::fclose(f);
    av_frame_free(&rgb);
}

/* ---------- main ---------- */

int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::cerr << "uso: " << argv[0] << " video.mp4 numero_frame out.ppm\n";
        return EXIT_FAILURE;
    }
    av_log_set_level(AV_LOG_QUIET);   // menos barulho

    VideoFile vf(argv[1]);
    AVFrame* fr = get_nth_frame(vf, std::stoul(argv[2]));
    if (!fr) {
        std::cerr << "frame não encontrado\n";
        return EXIT_FAILURE;
    }
    save_ppm(fr, argv[3]);
    std::cout << "frame salvo em " << argv[3] << '\n';
    return EXIT_SUCCESS;
}

