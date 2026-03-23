#ifndef CSECTIONCONFIG_H
#define CSECTIONCONFIG_H

class CSectionConfig
{
public:
    CSectionConfig();
//对应live2d  不同的版本
    enum Version{
        V3_0,
        V4_0,
        V4_2,
        V5_0
    };

};

#endif // CSECTIONCONFIG_H
