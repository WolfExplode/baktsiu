#ifndef BAKTSIU_STATIC_IMAGE_SOURCE_H_
#define BAKTSIU_STATIC_IMAGE_SOURCE_H_

#include "media_source.h"
#include "texture.h"

namespace baktsiu
{

// A static (non-video) image loaded from disk and resident on the GPU.
// Owns a Texture and exposes color grading parameters via MediaSource.
class StaticImageSource : public MediaSource
{
private:
    static uint8_t sSerialNo;

public:
    StaticImageSource(TextureSPtr& tex, uint8_t id = 0);

    GLuint      texId()    const override;
    Vec2f       size()     const override;
    std::string filename() const override;
    std::string filepath() const override;
    uint8_t     id()       const override;

    // Returns the underlying Texture; nullptr if not yet loaded.
    Texture*    getTexture() const;

    bool        reload();

private:
    TextureSPtr mTexture;
    uint8_t     mId = 0;
};

}  // namespace baktsiu
#endif // BAKTSIU_STATIC_IMAGE_SOURCE_H_
