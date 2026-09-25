#ifndef CPARENTGRAPH_H
#define CPARENTGRAPH_H

#include <QVector>

namespace live2dstruct {

class CParentGraph
{
public:
    enum class Error : quint8 { None, InvalidParent, Cycle, InvalidAncestor };

    CParentGraph() = default;
    explicit CParentGraph(const QVector<int>& parents) { Analyze(parents); }
    bool Analyze(const QVector<int>& parents);
    bool IsValid() const { return m_order.size() == m_parents.size(); }
    bool IsValid(int node) const;
    Error ErrorFor(int node) const;
    bool IsAncestor(int ancestor, int node) const;
    const QVector<int>& Parents() const { return m_parents; }
    const QVector<Error>& Errors() const { return m_errors; }
    const QVector<int>& Roots() const { return m_roots; }
    const QVector<int>& TopologicalOrder() const { return m_order; }
    const QVector<int>& FirstChild() const { return m_firstChild; }
    const QVector<int>& NextSibling() const { return m_nextSibling; }

private:
    QVector<int> m_parents;
    QVector<Error> m_errors;
    QVector<int> m_roots;
    QVector<int> m_order;
    QVector<int> m_firstChild;
    QVector<int> m_nextSibling;
    QVector<int> m_preorder;
    QVector<int> m_subtreeEnd;
};

}

#endif
