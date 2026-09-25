#include "cparentgraph.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace live2dstruct {

bool CParentGraph::Analyze(const QVector<int>& parents)
{
    if (parents.size() > std::numeric_limits<int>::max()) throw std::length_error("parent graph index range");
    const int count = int(parents.size());
    CParentGraph candidate;
    candidate.m_parents = parents;
    candidate.m_errors.fill(Error::None, count);
    candidate.m_firstChild.fill(-1, count);
    candidate.m_nextSibling.fill(-1, count);
    candidate.m_preorder.fill(-1, count);
    candidate.m_subtreeEnd.fill(-1, count);
    QVector<quint8> states(count, 0);
    QVector<int> positions(count, -1), path;
    for (int start = 0; start < count; ++start) {
        if (states[start]) continue;
        path.clear();
        int current = start;
        while (current != -1) {
            if (states[current] == 2) break;
            if (states[current] == 1) {
                for (int index = positions[current]; index < path.size(); ++index)
                    candidate.m_errors[path[index]] = Error::Cycle;
                break;
            }
            states[current] = 1;
            positions[current] = path.size();
            path.append(current);
            const int parent = parents[current];
            if (parent < -1 || parent >= count) {
                candidate.m_errors[current] = Error::InvalidParent;
                break;
            }
            current = parent;
        }
        for (auto node = path.crbegin(); node != path.crend(); ++node) {
            const int parent = parents[*node];
            if (candidate.m_errors[*node] == Error::None && parent != -1 &&
                candidate.m_errors[parent] != Error::None)
                candidate.m_errors[*node] = Error::InvalidAncestor;
            states[*node] = 2;
        }
    }
    for (int node = count - 1; node >= 0; --node) {
        if (candidate.m_errors[node] != Error::None || parents[node] == -1) continue;
        candidate.m_nextSibling[node] = candidate.m_firstChild[parents[node]];
        candidate.m_firstChild[parents[node]] = node;
    }
    for (int node = 0; node < count; ++node)
        if (parents[node] == -1) candidate.m_roots.append(node);
    candidate.m_order = candidate.m_roots;
    for (int next = 0; next < candidate.m_order.size(); ++next) {
        const int parent = candidate.m_order[next];
        for (int child = candidate.m_firstChild[parent]; child != -1; child = candidate.m_nextSibling[child])
            candidate.m_order.append(child);
    }
    int cursor = 0;
    for (int root : candidate.m_roots) {
        int current = root;
        while (current != -1) {
            candidate.m_preorder[current] = cursor++;
            if (candidate.m_firstChild[current] != -1) {
                current = candidate.m_firstChild[current];
                continue;
            }
            while (current != -1) {
                candidate.m_subtreeEnd[current] = cursor;
                if (candidate.m_nextSibling[current] != -1) {
                    current = candidate.m_nextSibling[current];
                    break;
                }
                current = parents[current];
            }
        }
    }
    *this = std::move(candidate);
    return IsValid();
}

bool CParentGraph::IsValid(int node) const
{
    return node >= 0 && node < m_errors.size() && m_errors[node] == Error::None;
}

CParentGraph::Error CParentGraph::ErrorFor(int node) const
{
    return node >= 0 && node < m_errors.size() ? m_errors[node] : Error::InvalidParent;
}

bool CParentGraph::IsAncestor(int ancestor, int node) const
{
    return ancestor != node && IsValid(ancestor) && IsValid(node) &&
        m_preorder[ancestor] <= m_preorder[node] && m_preorder[node] < m_subtreeEnd[ancestor];
}

}
