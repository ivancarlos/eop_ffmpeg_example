/*
 *  EOP-style single-frame extractor (versão corrigida)
 *  g++ (ou cmake) + FFmpeg
 *  Uso: ./get_frame video.mp4 150 out.ppm
 */

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

/* ---------- Conceitos (EOP) ---------- */

// T satisfaz FrameSource se possuir:
//   - bool   open()
//   - Frame* read()   // retorna ponteiro "dono" do frame (caller
//   libera)
//   - void  close()

/* ---------- Algoritmo genérico ---------- */

template <typename Src>
AVFrame* get_nth_frame(Src& src, std::size_t n)
{
    AVFrame* current = nullptr;

    for(std::size_t i = 0; i <= n; ++i)
    {
        AVFrame* next =
            src.read();  // alocado dentro de Src::read()
        if(!next)
        {
            // EOF antes de chegar no n-ésimo frame
            if(current)
                av_frame_free(&current);
            return nullptr;
        }
        // descartamos o anterior, ficamos só com o mais recente
        if(current)
            av_frame_free(&current);
        current = next;
    }
    return current;  // caller é dono e deve dar
                     // av_frame_free(&current)
}

/* ---------- Modelo concreto que satisfaz FrameSource ---------- */

class VideoFile
{
  public:
    explicit VideoFile(const std::string& path) : path_(path)
    {
    }

    bool open()
    {
        if(avformat_open_input(
               &fmt_, path_.c_str(), nullptr, nullptr) < 0)
            return false;

        if(avformat_find_stream_info(fmt_, nullptr) < 0)
            return false;

        // encontra o primeiro stream de vídeo
        for(unsigned i = 0; i < fmt_->nb_streams; ++i)
        {
            if(fmt_->streams[i]->codecpar->codec_type ==
               AVMEDIA_TYPE_VIDEO)
            {
                stream_index_ = static_cast<int>(i);
                break;
            }
        }
        if(stream_index_ == -1)
            return false;

        const AVCodec* codec = avcodec_find_decoder(
            fmt_->streams[stream_index_]->codecpar->codec_id);
        if(!codec)
            return false;

        codec_ctx_ = avcodec_alloc_context3(codec);
        if(!codec_ctx_)
            return false;

        if(avcodec_parameters_to_context(
               codec_ctx_, fmt_->streams[stream_index_]->codecpar) <
           0)
            return false;

        if(avcodec_open2(codec_ctx_, codec, nullptr) < 0)
            return false;

        pkt_ = av_packet_alloc();
        if(!pkt_)
            return false;

        return true;
    }

    // Lê próximo frame de vídeo.
    // Retorna:
    //   - AVFrame* alocado (caller deve av_frame_free)
    //   - nullptr em EOF ou erro
    AVFrame* read()
    {
        while(av_read_frame(fmt_, pkt_) >= 0)
        {
            if(pkt_->stream_index != stream_index_)
            {
                av_packet_unref(pkt_);
                continue;
            }

            int ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);
            if(ret < 0)
            {
                // erro ao enviar o pacote, tenta próximo
                continue;
            }

            AVFrame* frame = av_frame_alloc();
            if(!frame)
            {
                return nullptr;
            }

            ret = avcodec_receive_frame(codec_ctx_, frame);
            if(ret == AVERROR(EAGAIN))
            {
                // decoder precisa de mais dados; descarta esse
                // frame e continua
                av_frame_free(&frame);
                continue;
            }
            if(ret == AVERROR_EOF)
            {
                av_frame_free(&frame);
                return nullptr;
            }
            if(ret < 0)
            {
                av_frame_free(&frame);
                return nullptr;
            }

            // sucesso: devolve frame alocado
            return frame;
        }

        // chegou no EOF do arquivo
        return nullptr;
    }

    void close()
    {
        if(pkt_)
        {
            av_packet_free(&pkt_);
            pkt_ = nullptr;
        }
        if(codec_ctx_)
        {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
        if(fmt_)
        {
            avformat_close_input(&fmt_);
            fmt_ = nullptr;
        }
    }

    ~VideoFile()
    {
        close();
    }

  private:
    std::string      path_;
    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_ctx_{nullptr};
    AVPacket*        pkt_{nullptr};
    int              stream_index_{-1};
};

/* ---------- Salva frame como PPM ---------- */

void save_ppm(const AVFrame* fr, const std::string& out)
{
    if(!fr)
        return;

    FILE* f = std::fopen(out.c_str(), "wb");
    if(!f)
        throw std::runtime_error("cannot open output");

    // Converte para RGB24
    SwsContext* sws =
        sws_getContext(fr->width,
                       fr->height,
                       static_cast<AVPixelFormat>(fr->format),
                       fr->width,
                       fr->height,
                       AV_PIX_FMT_RGB24,
                       SWS_BILINEAR,
                       nullptr,
                       nullptr,
                       nullptr);

    if(!sws)
    {
        std::fclose(f);
        throw std::runtime_error("cannot create sws context");
    }

    AVFrame* rgb = av_frame_alloc();
    if(!rgb)
    {
        sws_freeContext(sws);
        std::fclose(f);
        throw std::runtime_error("cannot allocate rgb frame");
    }

    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = fr->width;
    rgb->height = fr->height;

    if(av_frame_get_buffer(rgb, 0) < 0)
    {
        av_frame_free(&rgb);
        sws_freeContext(sws);
        std::fclose(f);
        throw std::runtime_error("cannot allocate rgb buffer");
    }

    sws_scale(sws,
              fr->data,
              fr->linesize,
              0,
              fr->height,
              rgb->data,
              rgb->linesize);
    sws_freeContext(sws);

    std::fprintf(f, "P6\n%d %d\n255\n", fr->width, fr->height);
    for(int y = 0; y < fr->height; ++y)
    {
        std::fwrite(rgb->data[0] + y * rgb->linesize[0],
                    1,
                    fr->width * 3,
                    f);
    }

    std::fclose(f);
    av_frame_free(&rgb);
}

/* ---------- main ---------- */

int main(int argc, char* argv[])
{
    if(argc != 4)
    {
        std::cerr << "uso: " << argv[0]
                  << " video.mp4 numero_frame out.ppm\n";
        return EXIT_FAILURE;
    }

    av_log_set_level(AV_LOG_QUIET);  // menos barulho

    const std::string path    = argv[1];
    const std::size_t nth     = std::stoul(argv[2]);
    const std::string out_ppm = argv[3];

    VideoFile vf(path);
    if(!vf.open())
    {
        std::cerr << "não foi possível abrir o vídeo\n";
        return EXIT_FAILURE;
    }

    AVFrame* fr = get_nth_frame(vf, nth);
    if(!fr)
    {
        std::cerr << "frame não encontrado\n";
        vf.close();
        return EXIT_FAILURE;
    }

    try
    {
        save_ppm(fr, out_ppm);
        std::cout << "frame salvo em " << out_ppm << '\n';
    }
    catch(const std::exception& e)
    {
        std::cerr << "erro ao salvar frame: " << e.what() << '\n';
        av_frame_free(&fr);
        vf.close();
        return EXIT_FAILURE;
    }

    av_frame_free(&fr);
    vf.close();
    return EXIT_SUCCESS;
}
