#include "texture.h"

#include <algorithm>
#include <fstream>

#if defined(_WIN32) && defined(_MSC_VER)
// Without this, stbi__fopen uses fopen_s with the process ANSI code page. UTF-8 paths from
// the file dialog or drag-and-drop (e.g. Japanese filenames) then fail to open.
#define STBI_WINDOWS_UTF8
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#ifdef USE_OPENEXR
#include <ImathBox.h>
#include <ImfRgba.h>
#include <ImfRgbaFile.h>
#include <ImfTestFile.h>
#include <thread>
#endif

#ifdef USE_JXL
#include <jxl/decode.h>
#endif

#ifdef USE_AVIF
#include <avif/avif.h>
#endif

#ifdef USE_WEBP
#include <webp/decode.h>
#endif

#ifdef USE_TIFF
#include <tiffio.h>
#endif

#ifdef USE_SVG
#include <lunasvg.h>
#endif

#ifdef USE_DDS
#include <gli/gli.hpp>
#endif

namespace baktsiu
{

class AnimatedGifTexture final : public Texture
{
public:
    bool loadFromFile(const std::string& filepath) override
    {
        // Read file into memory so we can use stb's GIF API.
        std::ifstream in(filepath, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff sizeOff = in.tellg();
        if (sizeOff <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<stbi_uc> bytes(static_cast<size_t>(sizeOff));
        in.read(reinterpret_cast<char*>(bytes.data()), sizeOff);
        if (!in) {
            return false;
        }

        int* delays = nullptr;
        int x = 0, y = 0, z = 0, comp = 0;
        stbi_uc* allFrames = stbi_load_gif_from_memory(
            bytes.data(), static_cast<int>(bytes.size()), &delays, &x, &y, &z, &comp, 4);
        if (!allFrames || x <= 0 || y <= 0 || z <= 0) {
            if (allFrames) {
                stbi_image_free(allFrames);
            }
            if (delays) {
                STBI_FREE(delays);
            }
            return false;
        }

        const size_t frameBytes = static_cast<size_t>(x) * static_cast<size_t>(y) * 4u;
        mFrames.clear();
        mFrames.reserve(static_cast<size_t>(z));
        for (int i = 0; i < z; ++i) {
            const stbi_uc* src = allFrames + static_cast<size_t>(i) * frameBytes;
            mFrames.emplace_back(src, src + frameBytes);
        }

        mDelaysMs.assign(static_cast<size_t>(z), 0);
        if (delays) {
            for (int i = 0; i < z; ++i) {
                // stb stores GIF delays as milliseconds.
                const int d = delays[i];
                mDelaysMs[static_cast<size_t>(i)] = (std::max)(d, 10);
            }
            STBI_FREE(delays);
        } else {
            for (int i = 0; i < z; ++i) {
                mDelaysMs[static_cast<size_t>(i)] = 100;
            }
        }

        stbi_image_free(allFrames);

        mWidth = x;
        mHeight = y;
        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;

        mFilePath = filepath;
        mFileName = filepath.substr(filepath.find_last_of("/\\") + 1);

        mCurFrame = 0;
        mTimeBankMs = 0.0f;
        mUploadedFrame = -1;

        return true;
    }

    bool reloadFile() override
    {
        return loadFromFile(mFilePath) && upload();
    }

    bool upload() override
    {
        ScopeMarker(__FUNCTION__);

        if (mFrames.empty() || mWidth <= 0 || mHeight <= 0) {
            return false;
        }

        if (mTexId == 0) {
            glGenTextures(1, &mTexId);
        }

        glBindTexture(GL_TEXTURE_2D, mTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexStorage2D(GL_TEXTURE_2D, 1, mImageFormat, mWidth, mHeight);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mWidth, mHeight, GL_RGBA, mPixelDataType,
                        mFrames[0].data());

        mUploadedFrame = 0;
        return true;
    }

    void tick(float deltaTimeSec) override
    {
        if (mTexId == 0 || mFrames.size() <= 1) {
            return;
        }

        const float dtMs = deltaTimeSec * 1000.0f;
        mTimeBankMs += dtMs;

        // Advance frames; handle large dt by looping.
        int advanced = 0;
        while (mTimeBankMs >= static_cast<float>(mDelaysMs[mCurFrame]) && advanced < 64) {
            mTimeBankMs -= static_cast<float>(mDelaysMs[mCurFrame]);
            mCurFrame = (mCurFrame + 1) % static_cast<int>(mFrames.size());
            ++advanced;
        }

        if (mCurFrame == mUploadedFrame) {
            return;
        }

        glBindTexture(GL_TEXTURE_2D, mTexId);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mWidth, mHeight, GL_RGBA, mPixelDataType,
                        mFrames[static_cast<size_t>(mCurFrame)].data());
        mUploadedFrame = mCurFrame;
    }

    void release() override
    {
        Texture::release();
        mFrames.clear();
        mDelaysMs.clear();
        mCurFrame = 0;
        mUploadedFrame = -1;
        mTimeBankMs = 0.0f;
    }

private:
    std::vector<std::vector<uint8_t>> mFrames;
    std::vector<int> mDelaysMs;
    int mCurFrame = 0;
    int mUploadedFrame = -1;
    float mTimeBankMs = 0.0f;
};

#ifdef USE_DDS
class DdsTexture final : public Texture
{
public:
    bool loadFromFile(const std::string& filepath) override
    {
        gli::texture2d tex(gli::load_dds(filepath));
        if (tex.empty()) {
            return false;
        }

        mWidth  = tex.extent(0).x;
        mHeight = tex.extent(0).y;
        mGliTex = std::move(tex);

        mFilePath = filepath;
        mFileName = filepath.substr(filepath.find_last_of("/\\") + 1);
        return true;
    }

    bool reloadFile() override
    {
        mGliTex = gli::texture2d();
        return loadFromFile(mFilePath) && upload();
    }

    bool upload() override
    {
        if (mGliTex.empty()) {
            return false;
        }

        if (mTexId == 0) {
            glGenTextures(1, &mTexId);
        }

        glBindTexture(GL_TEXTURE_2D, mTexId);

        const bool hasMips = mGliTex.levels() > 1;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, hasMips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mGliTex.levels() - 1));

        gli::gl GL(gli::gl::PROFILE_GL33);
        const gli::gl::format fmt = GL.translate(mGliTex.format(), mGliTex.swizzles());
        const gli::texture2d::extent_type ext0 = mGliTex.extent(0);

        glTexStorage2D(GL_TEXTURE_2D, static_cast<GLint>(mGliTex.levels()), fmt.Internal, ext0.x, ext0.y);

        for (std::size_t level = 0; level < mGliTex.levels(); ++level) {
            const gli::texture2d::extent_type ext = mGliTex.extent(level);
            if (gli::is_compressed(mGliTex.format())) {
                glCompressedTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                    0, 0, ext.x, ext.y,
                    fmt.Internal,
                    static_cast<GLsizei>(mGliTex.size(level)),
                    mGliTex.data(0, 0, level));
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                    0, 0, ext.x, ext.y,
                    fmt.External, fmt.Type,
                    mGliTex.data(0, 0, level));
            }
        }

        mGliTex = gli::texture2d();
        return true;
    }

    void release() override
    {
        Texture::release();
        mGliTex = gli::texture2d();
    }

private:
    gli::texture2d mGliTex;
};
#endif  // USE_DDS

TextureSPtr createTextureForFile(const std::string& filepath)
{
    const ImageType type = Texture::getImageType(filepath);
    if (type == ImageType::GIF) {
        return std::make_shared<AnimatedGifTexture>();
    }
#ifdef USE_DDS
    if (type == ImageType::DDS) {
        return std::make_shared<DdsTexture>();
    }
#endif
    return std::make_shared<Texture>();
}

#ifdef USE_JXL
namespace {

bool loadJxlFromMemory(const std::vector<uint8_t>& bytes, int& width, int& height, uint8_t*& buffer)
{
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) {
        return false;
    }

    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return false;
    }

    if (JxlDecoderSetInput(dec, bytes.data(), bytes.size()) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return false;
    }
    JxlDecoderCloseInput(dec);

    const JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    bool gotImage = false;

    while (true) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
            break;
        }
        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
                break;
            }
            width = static_cast<int>(info.xsize);
            height = static_cast<int>(info.ysize);
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bufferSize = 0;
            if (JxlDecoderImageOutBufferSize(dec, &format, &bufferSize) != JXL_DEC_SUCCESS) {
                break;
            }
            buffer = static_cast<uint8_t*>(stbi__malloc(bufferSize));
            if (!buffer) {
                break;
            }
            if (JxlDecoderSetImageOutBuffer(dec, &format, buffer, bufferSize) != JXL_DEC_SUCCESS) {
                stbi_image_free(buffer);
                buffer = nullptr;
                break;
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            gotImage = true;
            break;
        }
    }

    JxlDecoderDestroy(dec);
    return gotImage && buffer != nullptr && width > 0 && height > 0;
}

}  // namespace
#endif

#ifdef USE_AVIF
namespace {

bool loadAvifFromMemory(const std::vector<uint8_t>& bytes, int& width, int& height, uint8_t*& buffer)
{
    avifDecoder* dec = avifDecoderCreate();
    if (!dec) {
        return false;
    }

    avifImage* image = avifImageCreateEmpty();
    if (!image) {
        avifDecoderDestroy(dec);
        return false;
    }

    avifResult result = avifDecoderReadMemory(dec, image, bytes.data(), bytes.size());
    if (result != AVIF_RESULT_OK) {
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        return false;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;

    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        return false;
    }

    if (avifImageYUVToRGB(image, &rgb) != AVIF_RESULT_OK) {
        avifRGBImageFreePixels(&rgb);
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        return false;
    }

    width  = static_cast<int>(rgb.width);
    height = static_cast<int>(rgb.height);
    const size_t bufSize = static_cast<size_t>(width) * height * 4;
    buffer = static_cast<uint8_t*>(stbi__malloc(bufSize));
    if (buffer) {
        memcpy(buffer, rgb.pixels, bufSize);
    }

    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(image);
    avifDecoderDestroy(dec);
    return buffer != nullptr && width > 0 && height > 0;
}

}  // namespace
#endif

#ifdef USE_WEBP
namespace {

bool loadWebpFromMemory(const std::vector<uint8_t>& bytes, int& width, int& height, uint8_t*& buffer)
{
    int w = 0, h = 0;
    if (!WebPGetInfo(bytes.data(), bytes.size(), &w, &h) || w <= 0 || h <= 0) {
        return false;
    }
    const size_t bufSize = static_cast<size_t>(w) * h * 4;
    buffer = static_cast<uint8_t*>(stbi__malloc(bufSize));
    if (!buffer) {
        return false;
    }
    if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), buffer, bufSize, w * 4)) {
        stbi_image_free(buffer);
        buffer = nullptr;
        return false;
    }
    width  = w;
    height = h;
    return true;
}

}  // namespace
#endif

#ifdef USE_TIFF
namespace {

bool loadTiffFromFile(const std::string& filepath, int& width, int& height, uint8_t*& buffer)
{
    TIFF* tif = TIFFOpen(filepath.c_str(), "r");
    if (!tif) {
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) {
        TIFFClose(tif);
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(w) * h;
    buffer = static_cast<uint8_t*>(stbi__malloc(pixelCount * 4));
    if (!buffer) {
        TIFFClose(tif);
        return false;
    }

    // TIFFReadRGBAImage fills bottom-up; flip rows to top-down.
    if (!TIFFReadRGBAImage(tif, w, h, reinterpret_cast<uint32_t*>(buffer), 0)) {
        stbi_image_free(buffer);
        buffer = nullptr;
        TIFFClose(tif);
        return false;
    }
    TIFFClose(tif);

    // Flip vertically (libtiff bottom-up → top-down).
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> tmp(rowBytes);
    for (uint32_t y = 0; y < h / 2; ++y) {
        uint8_t* top = buffer + y * rowBytes;
        uint8_t* bot = buffer + (h - 1 - y) * rowBytes;
        memcpy(tmp.data(), top, rowBytes);
        memcpy(top, bot, rowBytes);
        memcpy(bot, tmp.data(), rowBytes);
    }

    // libtiff RGBA is actually ABGR on little-endian; swizzle R↔B.
    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t* p = buffer + i * 4;
        std::swap(p[0], p[2]);
    }

    width  = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

}  // namespace
#endif

#ifdef USE_SVG
namespace {

bool loadSvgFromFile(const std::string& filepath, int& width, int& height, uint8_t*& buffer)
{
    auto document = lunasvg::Document::loadFromFile(filepath);
    if (!document) {
        return false;
    }

    auto bitmap = document->renderToBitmap();
    if (!bitmap.valid() || bitmap.width() == 0 || bitmap.height() == 0) {
        return false;
    }

    width  = static_cast<int>(bitmap.width());
    height = static_cast<int>(bitmap.height());
    const size_t pixelCount = static_cast<size_t>(width) * height;
    buffer = static_cast<uint8_t*>(stbi__malloc(pixelCount * 4));
    if (!buffer) {
        return false;
    }

    // lunasvg bitmap is ARGB32 premultiplied — convert to straight RGBA8.
    const uint32_t* src = reinterpret_cast<const uint32_t*>(bitmap.data());
    uint8_t* dst = buffer;
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t px = src[i];
        uint8_t a = static_cast<uint8_t>(px >> 24);
        uint8_t r = static_cast<uint8_t>(px >> 16);
        uint8_t g = static_cast<uint8_t>(px >>  8);
        uint8_t b = static_cast<uint8_t>(px);
        if (a > 0 && a < 255) {
            r = static_cast<uint8_t>((r * 255u) / a);
            g = static_cast<uint8_t>((g * 255u) / a);
            b = static_cast<uint8_t>((b * 255u) / a);
        }
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = a;
    }

    return true;
}

}  // namespace
#endif

bool Texture::isSupported(const std::string& filepath)
{
    return getImageType(filepath) != ImageType::Unknown;
}

ImageType Texture::getImageType(const std::string &filepath)
{
    ImageType type = ImageType::Unknown;

    FILE *f = stbi__fopen(filepath.c_str(), "rb");
    if (!f) return type;

#ifdef USE_JXL
    {
        uint8_t header[32] = {};
        const size_t readCount = fread(header, 1, sizeof(header), f);
        const JxlSignature sig = JxlSignatureCheck(header, readCount);
        if (sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER) {
            fclose(f);
            return ImageType::JXL;
        }
    }
#endif

#ifdef USE_AVIF
    {
        if (fseek(f, 0, SEEK_SET) == 0) {
            uint8_t header[12] = {};
            if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
                // ISOBMFF: bytes 4-7 must be "ftyp", bytes 8-11 are the major brand.
                if (memcmp(header + 4, "ftyp", 4) == 0) {
                    const char* brand = reinterpret_cast<const char*>(header + 8);
                    if (memcmp(brand, "avif", 4) == 0 || memcmp(brand, "avis", 4) == 0 ||
                        memcmp(brand, "MA1A", 4) == 0 || memcmp(brand, "MA1B", 4) == 0) {
                        fclose(f);
                        return ImageType::AVIF;
                    }
                }
            }
        }
    }
#endif

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return type;
    }

    stbi__context s;
    stbi__start_file(&s, f);

    if (stbi__jpeg_test(&s)) {
        type = ImageType::JPG;
    } else if (stbi__png_test(&s)) {
        type = ImageType::PNG;
    } else if (stbi__bmp_test(&s)) {
        type = ImageType::BMP;
    } else if (stbi__gif_test(&s)) {
        type = ImageType::GIF;
    } else if (stbi__hdr_test(&s)) {
        type = ImageType::HDR;
    } else if (stbi__tga_test(&s)) {
        type = ImageType::TGA;
    }

    if (type != ImageType::Unknown) {
        fclose(f);
        return type;
    }

#ifdef USE_OPENEXR
    if (Imf::isOpenExrFile(filepath.c_str())) {
        type = ImageType::OPENEXR;
    }
#endif

#ifdef USE_WEBP
    if (type == ImageType::Unknown) {
        if (fseek(f, 0, SEEK_SET) == 0) {
            uint8_t header[12] = {};
            if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
                // WebP: "RIFF" at 0, "WEBP" at 8.
                if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WEBP", 4) == 0) {
                    type = ImageType::WEBP;
                }
            }
        }
    }
#endif

#ifdef USE_TIFF
    if (type == ImageType::Unknown) {
        if (fseek(f, 0, SEEK_SET) == 0) {
            uint8_t header[4] = {};
            if (fread(header, 1, sizeof(header), f) >= 4) {
                // TIFF little-endian: "II" 0x2A 0x00 or big-endian: "MM" 0x00 0x2A
                if ((header[0] == 'I' && header[1] == 'I' && header[2] == 0x2A && header[3] == 0x00) ||
                    (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 && header[3] == 0x2A)) {
                    type = ImageType::TIFF;
                }
            }
        }
    }
#endif

    if (type == ImageType::Unknown) {
        if (fseek(f, 0, SEEK_SET) == 0) {
            uint8_t header[4] = {};
            if (fread(header, 1, sizeof(header), f) >= 4) {
                if (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x01 && header[3] == 0x00) {
                    type = ImageType::ICO;
                }
            }
        }
    }

#ifdef USE_SVG
    if (type == ImageType::Unknown) {
        if (fseek(f, 0, SEEK_SET) == 0) {
            char buf[256] = {};
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            if (strstr(buf, "<svg") != nullptr || strstr(buf, "<SVG") != nullptr) {
                type = ImageType::SVG;
            }
        }
    }
#endif

#ifdef USE_DDS
    if (type == ImageType::Unknown) {
        if (fseek(f, 0, SEEK_SET) == 0) {
            uint8_t header[4] = {};
            if (fread(header, 1, sizeof(header), f) >= 4) {
                // DDS magic: "DDS " (0x44 0x44 0x53 0x20)
                if (header[0] == 0x44 && header[1] == 0x44 && header[2] == 0x53 && header[3] == 0x20) {
                    type = ImageType::DDS;
                }
            }
        }
    }
#endif

    fclose(f);
    return type;
}

//-----------------------------------------------------------------------------

Texture::~Texture()
{
    release();
}

bool Texture::loadFromFile(const std::string& filepath)
{
    ImageType imageType = getImageType(filepath);

    uint8_t* buffer = nullptr;
    if (imageType == ImageType::HDR) {
        PushRangeMarker(__FUNCTION__);
        buffer = reinterpret_cast<uint8_t*>(stbi_loadf(filepath.c_str(), &mWidth, &mHeight, &mChannelNum, 4));
        mPixelDataType = GL_FLOAT;
        mImageFormat = GL_RGBA16F;
        PopRangeMarker();
    }
#ifdef USE_OPENEXR
    else if (imageType == ImageType::OPENEXR) {
        Imf::setGlobalThreadCount(std::thread::hardware_concurrency());

        Imf::RgbaInputFile file(filepath.c_str());
        Imath::Box2i dw = file.dataWindow();

        mWidth = dw.max.x - dw.min.x + 1;
        mHeight = dw.max.y - dw.min.y + 1;
        mChannelNum = 4;
        buffer = (uint8_t*)stbi__malloc_mad4(mWidth, mHeight, mChannelNum, sizeof(Imf::Rgba), 0);

        file.setFrameBuffer((Imf::Rgba*)buffer - dw.min.x - dw.min.y * mWidth, 1, mWidth);
        file.readPixels(dw.min.y, dw.max.y);
        
        mPixelDataType = GL_HALF_FLOAT;
        mImageFormat = GL_RGBA16F;
    }
#endif
#ifdef USE_JXL
    else if (imageType == ImageType::JXL) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff sizeOff = in.tellg();
        if (sizeOff <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(static_cast<size_t>(sizeOff));
        in.read(reinterpret_cast<char*>(bytes.data()), sizeOff);
        if (!in) {
            return false;
        }

        if (!loadJxlFromMemory(bytes, mWidth, mHeight, buffer)) {
            return false;
        }

        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }
#endif
#ifdef USE_AVIF
    else if (imageType == ImageType::AVIF) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff sizeOff = in.tellg();
        if (sizeOff <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(static_cast<size_t>(sizeOff));
        in.read(reinterpret_cast<char*>(bytes.data()), sizeOff);
        if (!in) {
            return false;
        }

        if (!loadAvifFromMemory(bytes, mWidth, mHeight, buffer)) {
            return false;
        }

        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }
#endif
#ifdef USE_WEBP
    else if (imageType == ImageType::WEBP) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff sizeOff = in.tellg();
        if (sizeOff <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(static_cast<size_t>(sizeOff));
        in.read(reinterpret_cast<char*>(bytes.data()), sizeOff);
        if (!in) {
            return false;
        }

        if (!loadWebpFromMemory(bytes, mWidth, mHeight, buffer)) {
            return false;
        }

        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }
#endif
#ifdef USE_TIFF
    else if (imageType == ImageType::TIFF) {
        if (!loadTiffFromFile(filepath, mWidth, mHeight, buffer)) {
            return false;
        }

        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }
#endif
#ifdef USE_SVG
    else if (imageType == ImageType::SVG) {
        if (!loadSvgFromFile(filepath, mWidth, mHeight, buffer)) {
            return false;
        }

        mChannelNum = 4;
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }
#endif
    else {
        buffer = stbi_load(filepath.c_str(), &mWidth, &mHeight, &mChannelNum, 4);
        mPixelDataType = GL_UNSIGNED_BYTE;
        mImageFormat = GL_RGBA8;
    }

    if (!buffer) {
        return false;
    }

    if (mBuffer) {
        // Remove old texture data.
        stbi_image_free(mBuffer);
    }

    mBuffer = buffer;

    mFilePath = filepath;
    mFileName = filepath.substr(filepath.find_last_of("/") + 1);

    return true;
}

bool Texture::reloadFile()
{
    return loadFromFile(mFilePath) && upload();
}

bool Texture::upload()
{
    ScopeMarker(__FUNCTION__);

    if (!mBuffer) {
        return false;
    }

    if (mTexId == 0) {
        // Create a OpenGL texture identifier
        glGenTextures(1, &mTexId);
    }

    glBindTexture(GL_TEXTURE_2D, mTexId);

    // Setup filtering parameters for display
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Upload pixels into texture
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mWidth, mHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, mBuffer);

    glTexStorage2D(GL_TEXTURE_2D, 1, mImageFormat, mWidth, mHeight);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mWidth, mHeight, GL_RGBA, mPixelDataType, mBuffer);

    stbi_image_free(mBuffer);
    mBuffer = nullptr;

    return true;
}

void Texture::release()
{
    if (mBuffer) {
        stbi_image_free(mBuffer);
        mBuffer = nullptr;
    }

    if (mTexId) {
        glDeleteTextures(1, &mTexId);
        mTexId = 0;
    }
}

void    Texture::bind()
{
    glBindTexture(GL_TEXTURE_2D, mTexId);
}

void    Texture::unbind()
{
    glBindTexture(GL_TEXTURE_2D, 0);
}

//-----------------------------------------------------------------------------

bool    RenderTexture::bindAsOutput(const Vec2i& size, GLenum imageFormat)
{
    if (mSize == size && mImageFormat == imageFormat) {
        glBindFramebuffer(GL_FRAMEBUFFER, mFboId);
        return true;
    }
    
    if (mTexId == 0) {
        glGenTextures(1, &mTexId);
    }

    glBindTexture(GL_TEXTURE_2D, mTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, imageFormat, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (mFboId == 0) {
        glGenFramebuffers(1, &mFboId);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, mFboId);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexId, 0);

#ifdef _DEBUG
    bool isValid = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (!isValid) {
        return false;
    }
#endif

    mSize = size;
    mImageFormat = imageFormat;

    return true;
}

void    RenderTexture::release()
{
    glDeleteTextures(1, &mTexId);
    glDeleteFramebuffers(1, &mFboId);

    mTexId = 0;
    mFboId = 0;
}

void    RenderTexture::bindAsInput(bool useLinearFilter)
{
    glBindTexture(GL_TEXTURE_2D, mTexId);

    if (useLinearFilter == mUseLinearFilter) {
        return;
    }

    // Setup filtering parameters for display
    GLint filterType = useLinearFilter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterType);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterType);
    mUseLinearFilter = useLinearFilter;
}

void    RenderTexture::unbind()
{
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

//-----------------------------------------------------------------------------

bool    Sampler::initialize(GLenum minFilter, GLenum magFilter)
{
    if (mId == 0) {
        glGenSamplers(1, &mId);
    }

    glSamplerParameteri(mId, GL_TEXTURE_MIN_FILTER, minFilter);
    glSamplerParameteri(mId, GL_TEXTURE_MAG_FILTER, magFilter);
    glSamplerParameteri(mId, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(mId, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(mId, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return mId > 0;
}

void    Sampler::release()
{
    glDeleteSamplers(1, &mId);
}

void    Sampler::bind(GLuint unit)
{
    glBindSampler(unit, mId);
}

void    Sampler::unbind(GLuint unit)
{
    glBindSampler(unit, 0);
}


}  // namespace baktsiu