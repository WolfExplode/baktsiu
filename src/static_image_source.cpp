#include "static_image_source.h"

namespace baktsiu
{

uint8_t StaticImageSource::sSerialNo = 0;

StaticImageSource::StaticImageSource(TextureSPtr& tex, uint8_t id)
    : mTexture(tex)
{
    mId = (id == 0) ? ++sSerialNo : id;
}

GLuint StaticImageSource::texId() const
{
    return mTexture ? mTexture->id() : 0;
}

Vec2f StaticImageSource::size() const
{
    return mTexture ? mTexture->size() : Vec2f(1.0f);
}

std::string StaticImageSource::filename() const
{
    return mTexture ? mTexture->filename() : "none";
}

std::string StaticImageSource::filepath() const
{
    return mTexture ? mTexture->filepath() : "none";
}

uint8_t StaticImageSource::id() const
{
    return mId;
}

Texture* StaticImageSource::getTexture() const
{
    return mTexture ? mTexture.get() : nullptr;
}

bool StaticImageSource::reload()
{
    return mTexture ? mTexture->reloadFile() : false;
}

}  // namespace baktsiu
