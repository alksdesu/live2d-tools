#ifndef CDRAWABLEFLAGS_H
#define CDRAWABLEFLAGS_H

namespace live2dstruct {

class CDrawableFlags
{
public:
    static constexpr int BlendMode(int flags) { return flags & 0x1 ? 1 : (flags & 0x2 ? 2 : 0); }
    static constexpr bool Culling(int flags) { return (flags & 0x4) == 0; }
    static constexpr bool MaskInverted(int flags) { return (flags & 0x8) != 0; }
};

}

#endif
