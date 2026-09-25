#ifndef CROTATIONNODEKEYFORM_H
#define CROTATIONNODEKEYFORM_H
#include "ckeyformobject.h"
#include <vector>
namespace live2dstruct {
class CRotationNodeKeyform : public CKeyFormObject
{

public:
    CRotationNodeKeyform(float opacities,float angles,float origin_x,float origin_y,float scales,bool is_reflect_x,bool is_reflect_y);
    CRotationNodeKeyform();
    virtual ~CRotationNodeKeyform();


    std::vector<float>& GetPosition(){
        return keyform_position;
    }

    void SetPosition(std::vector<float>& pos){
        keyform_position =pos;
    }


    float Getopacities(){
        return opacities;
    }
    float Getangles(){
        return angles;
    }
    float Getscales(){
        return scales;
    }
    float Getorigin_x(){
        return keyform_position[0];
    }
    float Getorigin_y(){
        return keyform_position[1];
    }
    bool Getis_reflect_x(){
        return is_reflect_x;
    }
    bool Getis_reflect_y(){
        return is_reflect_y;
    }



    void Setopacities(float opacities){
        this->opacities = opacities;
    }
    void Setangles(float angles){
        this->angles = angles;
    }
    void Setscales(float scales){
        this->scales = scales;
    }
    void Setorigin_x(float x){
        this->keyform_position[0] = x;
    }
    void Setorigin_y(float y){
        this->keyform_position[1] = y;
    }
    void Setis_reflect_x(bool is_reflect_x){
        this->is_reflect_x = is_reflect_x;
    }
    void Setis_reflect_y(bool is_reflect_y){
        this->is_reflect_y = is_reflect_y;
    }

private:

    float opacities = 1.0f;
    float angles = 0.0f;
    float scales = 1.0f;
    bool is_reflect_x = false;
    bool is_reflect_y = false;
    std::vector<float> keyform_position;


};
}
#endif // CROTATIONNODEKEYFORM_H
