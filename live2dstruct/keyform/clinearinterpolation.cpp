#include "clinearinterpolation.h"
namespace live2dstruct {
CLinearInterpolation::CLinearInterpolation() {}

void CLinearInterpolation::SetCKeyFormObject(C2DTreeNode *node)
{

    QList<C2DTreeNode*>& list = node->getFathers();
    std::vector<CParameterNode*> vetPara;
    for(int i = 0 ;i < list.size() ;i++){
        if(dynamic_cast<CParameterNode*>(list[i])){
            CParameterNode* para = (CParameterNode*)list[i];
            if(para->GetType()==0){
                vetPara.push_back(para);//
            }

        }
    }
    //
    if(vetPara.size() == 1){
        bool isScee = CalOnePara(node,vetPara);
        if(!isScee){
            CalZorePara(node);
        }
    }
    else if(vetPara.size() == 2){
        bool isScee = CalTowPara(node,vetPara);
        if(!isScee){
            CalZorePara(node);
        }
    }else if(vetPara.size() == 0){
        CalZorePara(node);
    }

}
//单线性插值的数学原理
bool CLinearInterpolation::CalOnePara(C2DTreeNode *node ,std::vector<CParameterNode*>& vetPara)
{
    //qDebug() << "CLinearInterpolation::CalOnePara";
    float curValue =vetPara[0]->GetCurValue();
    const std::vector<float>&  keys = vetPara[0]->GetKeys();
    int index = -1;
    if(keys.size() > 1){

        for(int i = 0 ;i < keys.size() - 1;i++){
            if(keys[i] <=curValue && curValue <= keys[i+1]){
                index = i;
                break;
            }
        }

    }else{

        return false;
    }


    if(index != -1){
        float labd =(curValue - keys[index]) /( keys[index+1] - keys[index]);

        if(dynamic_cast<CRotationNode*>(node)){
            CRotationNode* roRep = (CRotationNode*) node;
            std::vector<CRotationNodeKeyform>& vetForm = roRep->GetKeyFormS();
            if(vetForm.size() <= index+1){
                qDebug()<< "CRotationNode*越界";
                return false;
            }
            std::vector<float>& src = vetForm[index].GetPosition();
            std::vector<float>& des = vetForm[index+1].GetPosition();
            std::vector<float> k;
            k.resize(src.size(),0.0f);//位置发生变换
            for(int i = 0;i < k.size();i++){
                k[i] =GetOneInsertFun(labd,des[i],src[i]);
            }

            float srcAngel = vetForm[index].Getangles();
            float desAngel = vetForm[index+1].Getangles();
            float kAngel =GetOneInsertFun(labd,desAngel,srcAngel);


            float srcScales =  vetForm[index].Getscales();
            float desScales =  vetForm[index+1].Getscales();
            float kScales  =GetOneInsertFun(labd,desScales,srcScales);



            CRotationNodeKeyform& curKey = roRep->GetCurKeyForm();
            curKey.SetPosition(k);
            curKey.Setangles(kAngel);
            curKey.Setscales(kScales);

        }
        else if(dynamic_cast<CWarpNode*>(node)){
            CWarpNode* warpNode = (CWarpNode*) node;
            std::vector<CWarpNodeKeyform>& vetForm = warpNode->GetKeyFormS();

            if(vetForm.size() <= index+1){
                qDebug()<< "CWarpNode*越界";
                return false;
            }
            std::vector<float>& src = vetForm[index].GetPosition();
            std::vector<float>& des = vetForm[index+1].GetPosition();
            std::vector<float> k;
            k.resize(src.size(),0.0f);
            //透明度发生变换
            for(int i = 0;i < k.size();i++){
                k[i] = GetOneInsertFun(labd,des[i],src[i]);
            }

            CWarpNodeKeyform& key= warpNode->GetCurKeyForm();
            key.SetPosition(k);

        }
        else if(dynamic_cast<CArtMeshNode*>(node)){
            //当前帧不计算

            CArtMeshNode* artNode = (CArtMeshNode*) node;

            std::vector<CArtMeshKeyForm>& vetForm = artNode->GetKeyFormS();
            if(vetForm.size() <= index+1){
                qDebug()<< "CArtMeshNode*越界";
                return false;
            }
            std::vector<float>& src = vetForm[index].GetPosition();
            std::vector<float>& des = vetForm[index+1].GetPosition();
            std::vector<float> k;
            k.resize(src.size(),0.0f);
            //透明度发生变换
            for(int i = 0;i < k.size();i++){
                k[i] = GetOneInsertFun(labd,des[i],src[i]);
            }
            CArtMeshKeyForm& key= artNode->GetCurKeyForm();
            key.SetPosition(k);
        }

    }

    return true;
}


//双线性插值的数学原理
//加权求和
bool CLinearInterpolation::CalTowPara(C2DTreeNode *node, std::vector<CParameterNode*> &vetPara)
{

    //qDebug() << "CLinearInterpolation::CalTowPara";
    //现在的问题是怎么确认参数帧位置
    float curParValue1 = vetPara[0]->GetCurValue();
    float curParValue2 = vetPara[1]->GetCurValue();
    int index1 = -1;
    int index2 = -1;
    const std::vector<float>&  keys1 = vetPara[0]->GetKeys();
    const std::vector<float>&  keys2 = vetPara[1]->GetKeys();
    if(keys1.size() >1 && keys2.size() > 1){
        for(int i = 0 ;i < keys1.size() - 1;i++){
            if(keys1[i] <= curParValue1 && curParValue1 <= keys1[i+1]){
                index1 = i;
                break;
            }
        }
        for(int i = 0 ;i < keys2.size() - 1;i++){
            if(keys2[i] <= curParValue2 && curParValue2 <= keys2[i+1]){
                index2 = i;
                break;
            }
        }

    }
    else{
        return false;
    }



    //index1 = 0 ,index2 = 0
    if(index1 != -1 && index2 != -1){
        float labd1 =(curParValue1 - keys1[index1]) /(keys1[index1+1] - keys1[index1] );
        float labd2 =(curParValue2 - keys2[index2]) /(keys2[index2+1] - keys2[index2] );
        int offset = keys1.size();
        int offsetx1=(index2 * offset) + index1 ;
        int offsety1=(index2 * offset) + index1+1 ;
        int offsetx2=((index2+1) * offset) + index1 ;//7
        int offsety2=((index2+1) * offset) + index1+1 ;//8

        //旋转的线性插值
        if(dynamic_cast<CRotationNode*>(node)){
            CRotationNode* roRep = (CRotationNode*) node;
            std::vector<CRotationNodeKeyform>& vetForm = roRep->GetKeyFormS();
            if(offsety2 >= vetForm.size()){
                qDebug() << "dynamic_cast<CRotationNode*>(node) 越界" << roRep->getName();
                return false;
            }
            std::vector<float>& src1 = vetForm[offsetx1].GetPosition();
            std::vector<float>& src2 = vetForm[offsety1].GetPosition();
            std::vector<float>& src3 = vetForm[offsetx2].GetPosition();
            std::vector<float>& src4 = vetForm[offsety2].GetPosition();

            std::vector<float> k;
            k.resize(src1.size(),0.0f);

            for(int i = 0;i < k.size();i++){
                k[i] = GetTowInsertFun(labd1,labd2,src1[i],src2[i],src3[i],src4[i]);//可以进行某个点的加权求和
            }

            float srcAngel1 = vetForm[offsetx1].Getangles();
            float srcAngel2 = vetForm[offsety1].Getangles();
            float srcAngel3 = vetForm[offsetx2].Getangles();
            float srcAngel4 = vetForm[offsety2].Getangles();
            float kAngel =  GetTowInsertFun(labd1,labd2,srcAngel1,srcAngel2,srcAngel3,srcAngel4);//可以进行某个点的加权求和

            float srcScales1 =  vetForm[offsetx1].Getscales();
            float srcScales2 =  vetForm[offsety1].Getscales();
            float srcScales3 =  vetForm[offsetx2].Getscales();
            float srcScales4 =  vetForm[offsety2].Getscales();
            float kScales = GetTowInsertFun(labd1,labd2,srcScales1,srcScales2,srcScales3,srcScales4);//可以进行某个点的加权求和

            CRotationNodeKeyform& curKey = roRep->GetCurKeyForm();
            curKey.SetPosition(k);
            curKey.Setangles(kAngel);
            curKey.Setscales(kScales);
        }
        else if(dynamic_cast<CWarpNode*>(node)){
            CWarpNode* warpNode = (CWarpNode*) node;
            std::vector<CWarpNodeKeyform>& vetForm = warpNode->GetKeyFormS();
            if(offsety2 >= vetForm.size()){
                qDebug() << "dynamic_cast<CRotationNode*>(node) 越界" << warpNode->getName();
                return false;
            }
            std::vector<float>& src1 = vetForm[offsetx1].GetPosition();
            std::vector<float>& src2 = vetForm[offsety1].GetPosition();
            std::vector<float>& src3 = vetForm[offsetx2 ].GetPosition();
            std::vector<float>& src4 = vetForm[offsety2].GetPosition();
            std::vector<float> k;
            k.resize(src1.size(),0.0f);
            for(int i = 0;i < k.size();i++){
                k[i] =GetTowInsertFun(labd1,labd2,src1[i],src2[i],src3[i],src4[i]);//可以进行某个点的加权求和
            }
            CWarpNodeKeyform& key= warpNode->GetCurKeyForm();
            key.SetPosition(k);

        }
        else if(dynamic_cast<CArtMeshNode*>(node)){
            //当前帧不计算

            CArtMeshNode* artNode = (CArtMeshNode*) node;
            std::vector<CArtMeshKeyForm>& vetForm = artNode->GetKeyFormS();
            if(offsety2 >= vetForm.size()){
                qDebug() << "dynamic_cast<CRotationNode*>(node) 越界" << artNode->getName();
                return false;
            }
            std::vector<float>& src1 = vetForm[offsetx1].GetPosition();
            std::vector<float>& src2 = vetForm[offsety1].GetPosition();
            std::vector<float>& src3 = vetForm[offsetx2].GetPosition();
            std::vector<float>& src4 = vetForm[offsety2].GetPosition();
            std::vector<float> k;
            k.resize(src1.size(),0.0f);
            //透明度发生变换
            for(int i = 0;i < k.size();i++){
                k[i] = GetTowInsertFun(labd1,labd2,src1[i],src2[i],src3[i],src4[i]);//可以进行某个点的加权求和
            }
            CArtMeshKeyForm& key= artNode->GetCurKeyForm();
            key.SetPosition(k);
        }

    }
    return true;

}

void CLinearInterpolation::CalZorePara(C2DTreeNode *node)
{
    //qDebug() << "CLinearInterpolation::CalZorePara";
    if(dynamic_cast<CRotationNode*>(node)){

        CRotationNode* roRep = (CRotationNode*) node;
        std::vector<CRotationNodeKeyform>& vetForm = roRep->GetKeyFormS();
        if(vetForm.size() > 0){
            roRep->SetCurKeyForm(vetForm[0]);
        }

    }
    else if(dynamic_cast<CWarpNode*>(node)){
        CWarpNode* warpNode = (CWarpNode*) node;
        std::vector<CWarpNodeKeyform>& vetForm = warpNode->GetKeyFormS();
        if(vetForm.size() > 0){
          warpNode->SetCurKeyForm(vetForm[0]);
        }


    }
    else if(dynamic_cast<CArtMeshNode*>(node)){
        //当前帧不计算
        CArtMeshNode* artNode = (CArtMeshNode*) node;
        std::vector<CArtMeshKeyForm>& vetForm = artNode->GetKeyFormS();
        if(vetForm.size() > 0){
            artNode->SetCurMeshKey(vetForm[0]);
        }


    }

}

float CLinearInterpolation::GetOneInsertFun(float a,float x0 , float x1)
{
    return   a* x0 + (1-a)*x1;
}

float CLinearInterpolation::GetTowInsertFun(float b, float a, float x0, float y0, float x1, float y1)
{
    return (1-b)*(1-a)*x0 + (1-b)*a*x1  + b*(1-a)*y0+ b*a*y1;
}

float CLinearInterpolation::GetTowFun(float x, float y)
{
    return (x+y)/2.0f;
}

}
