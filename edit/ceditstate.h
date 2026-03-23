#ifndef CEDITSTATE_H
#define CEDITSTATE_H

class  CEditState
{
public:
    enum CState{
        NotState = 0,
        MousePress = 0x1 << 0,
        ViewMove = 0x1 << 1,
        MouseRealse = 0x1 << 2,
        Zoom = 0x1 << 3,//放大状态
        Select = 0x1 << 4,//选择形状状态
        SelectRect = 0x1 << 5,//选择居心
        EidtMesh = 0x1 << 6,
        PrintPsd = 0x1 << 7,
        ModelOpen = 0x1 << 8,
        PrintAllPsd = 0x1 << 9,
        PrintFenzuPsd = 0x1 << 10,

    };
    CEditState(){
        m_curState = NotState;
    }

    void ChangeState(const CState& state){
        m_curState = state;

    }
    void AddState(const CState& des){
        m_curState = CState((int)m_curState | (int)des);
    }
    void RemoveState(const CState& des){
        m_curState = CState((int)m_curState & ~(int)des);
    }
    const CState& GetCurState(){
        return m_curState;
    }

    const bool IsContins(const CState& des){
        return CState((int)m_curState & (int)des);
    }

private:
    CState m_curState;


};



#endif // CEDITSTATE_H
