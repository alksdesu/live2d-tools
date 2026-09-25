#include "c2dtreenode.h"

namespace live2dstruct {

C2DTreeNode::C2DTreeNode(const QString& name) : name(name) {}
C2DTreeNode::~C2DTreeNode() = default;

void C2DTreeNode::SetParentFrame(std::shared_ptr<const CCoordinateFrame> frame)
{
    m_parentFrame = std::move(frame);
}

bool C2DTreeNode::MapToCanvas(const std::vector<float>& positions, std::vector<float>& output) const
{
    static const CCoordinateFrame identity(nullptr);
    return (m_parentFrame ? *m_parentFrame : identity).MapToCanvas(positions, output);
}

bool C2DTreeNode::MapFromCanvas(const std::vector<float>& positions, std::vector<float>& output) const
{
    static const CCoordinateFrame identity(nullptr);
    return (m_parentFrame ? *m_parentFrame : identity).MapFromCanvas(positions, output);
}

bool C2DTreeNode::UpdateCanvasPositions(const std::vector<float>& positions)
{
    if (MapToCanvas(positions, m_canvasPositions)) return true;
    m_canvasPositions.clear();
    return false;
}

}
