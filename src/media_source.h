#ifndef BAKTSIU_MEDIA_SOURCE_H_
#define BAKTSIU_MEDIA_SOURCE_H_

#include <GL/gl3w.h>

#include "colour.h"
#include "common.h"

#include <string>

namespace baktsiu
{

// Abstract base for anything that can be displayed in a viewport slot.
// Both static images and video frames satisfy this interface.
class MediaSource
{
public:
    virtual ~MediaSource() = default;

    virtual GLuint      texId() const = 0;
    virtual Vec2f       size() const = 0;
    virtual std::string filename() const = 0;
    virtual std::string filepath() const = 0;
    virtual uint8_t     id() const = 0;

    virtual bool        isVideo() const { return false; }
    virtual void        tick(double /*dt*/) {}

    ColorPrimaryType    getColorPrimaryType()  const { return mColorPrimaryType; }
    ColorEncodingType   getColorEncodingType() const { return mColorEncodingType; }
    void                setColorPrimaryType(ColorPrimaryType v)  { mColorPrimaryType  = v; }
    void                setColorEncodingType(ColorEncodingType v) { mColorEncodingType = v; }

protected:
    ColorPrimaryType    mColorPrimaryType  = ColorPrimaryType::sRGB;
    ColorEncodingType   mColorEncodingType = ColorEncodingType::sRGB;
};

}  // namespace baktsiu
#endif // BAKTSIU_MEDIA_SOURCE_H_
