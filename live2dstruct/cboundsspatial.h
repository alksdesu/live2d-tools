#ifndef CBOUNDSSPATIAL_H
#define CBOUNDSSPATIAL_H

#include <QVector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>

namespace live2dstruct {
class CBoundsSpatial
{
public:
    struct Box
    {
        double left, right, top, bottom;
        bool overlaps(const Box& other, bool interior = false) const
        {
            if (interior)
                return std::max(top, other.top) < std::min(bottom, other.bottom)
                    && std::max(left, other.left) < std::min(right, other.right);
            return top <= other.bottom && other.top <= bottom && left <= other.right && other.left <= right;
        }
    };

    enum class Usage { Pairs, Queries };

    template <class Work> CBoundsSpatial(const QVector<Box>& source, Work work, Usage usage = Usage::Pairs)
    {
        if (source.size() > 3000000)
            throw std::length_error("Spatial bounds exceed the item limit");
        boxes.reserve(source.size());
        for (const auto& box : source) {
            if (!std::isfinite(box.left) || !std::isfinite(box.right) || !std::isfinite(box.top)
                || !std::isfinite(box.bottom) || box.left > box.right || box.top > box.bottom)
                throw std::invalid_argument("Spatial bounds must be finite and ordered");
            boxes.append(box);
        }
        order.resize(boxes.size());
        std::iota(order.begin(), order.end(), 0);
        if (usage == Usage::Queries)
            nodes.reserve(boxes.size() * 2);
        if (usage == Usage::Queries && !boxes.isEmpty())
            build(0, order.size(), work);
    }

    template <class Work, class Visit> void pairs(bool interior, Work work, Visit visit) const
    {
        struct IntervalNode
        {
            double center;
            int left = -1, right = -1, parent = -1, active = 0;
            std::set<std::pair<double, int>> lower, upper;
        };
        QVector<double> endpoints;
        for (const auto& box : boxes) {
            endpoints.append(box.top);
            endpoints.append(box.bottom);
        }
        std::sort(endpoints.begin(), endpoints.end());
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        QVector<IntervalNode> tree;
        tree.reserve(endpoints.size());
        const auto buildTree = [&](auto&& self, int begin, int end, int parent) -> int {
            if (begin == end)
                return -1;
            work();
            const int middle = begin + (end - begin) / 2, index = tree.size();
            tree.append({ endpoints[middle], -1, -1, parent, 0, { }, { } });
            const int left = self(self, begin, middle, index);
            const int right = self(self, middle + 1, end, index);
            tree[index].left = left;
            tree[index].right = right;
            return index;
        };
        if (endpoints.isEmpty())
            return;
        buildTree(buildTree, 0, endpoints.size(), -1);
        QVector<int> owner(boxes.size()), starts = order, ends = order;
        for (int i = 0; i < boxes.size(); ++i) {
            work();
            int node = 0;
            while (boxes[i].bottom < tree[node].center || boxes[i].top > tree[node].center)
                node = boxes[i].bottom < tree[node].center ? tree[node].left : tree[node].right;
            owner[i] = node;
        }
        std::sort(starts.begin(), starts.end(),
            [&](int a, int b) { return boxes[a].left < boxes[b].left || (boxes[a].left == boxes[b].left && a < b); });
        std::sort(ends.begin(), ends.end(), [&](int a, int b) {
            return boxes[a].right < boxes[b].right || (boxes[a].right == boxes[b].right && a < b);
        });
        QVector<bool> active(boxes.size(), false);
        const auto update = [&](int item, bool add) {
            work();
            auto& node = tree[owner[item]];
            if (add) {
                node.lower.emplace(boxes[item].top, item);
                node.upper.emplace(-boxes[item].bottom, item);
            } else {
                node.lower.erase({ boxes[item].top, item });
                node.upper.erase({ -boxes[item].bottom, item });
            }
            active[item] = add;
            for (int index = owner[item]; index >= 0; index = tree[index].parent)
                tree[index].active += add ? 1 : -1;
        };
        const auto queryTree = [&](auto&& self, int index, int item) -> void {
            if (index < 0 || !tree[index].active)
                return;
            work();
            const auto& node = tree[index];
            const auto& box = boxes[item];
            const auto visitPair = [&](int other) {
                work();
                if (box.overlaps(boxes[other], interior))
                    visit(std::min(item, other), std::max(item, other));
            };
            if (box.bottom < node.center) {
                for (const auto& entry : node.lower) {
                    if (interior ? entry.first >= box.bottom : entry.first > box.bottom)
                        break;
                    visitPair(entry.second);
                }
                self(self, node.left, item);
            } else if (box.top > node.center) {
                for (const auto& entry : node.upper) {
                    if (interior ? -entry.first <= box.top : -entry.first < box.top)
                        break;
                    visitPair(entry.second);
                }
                self(self, node.right, item);
            } else {
                for (const auto& entry : node.lower)
                    visitPair(entry.second);
                self(self, node.left, item);
                self(self, node.right, item);
            }
        };
        int expired = 0;
        for (int item : starts) {
            work();
            while (expired < ends.size()
                && (interior ? boxes[ends[expired]].right <= boxes[item].left
                             : boxes[ends[expired]].right < boxes[item].left)) {
                if (active[ends[expired]])
                    update(ends[expired], false);
                ++expired;
            }
            if (interior && (boxes[item].left == boxes[item].right || boxes[item].top == boxes[item].bottom))
                continue;
            queryTree(queryTree, 0, item);
            update(item, true);
        }
    }

    template <class Work, class Visit> void query(const Box& box, Work work, Visit visit) const
    {
        if (!std::isfinite(box.left) || !std::isfinite(box.right) || !std::isfinite(box.top)
            || !std::isfinite(box.bottom) || box.left > box.right || box.top > box.bottom)
            throw std::invalid_argument("Query bounds must be finite and ordered");
        if (nodes.isEmpty() && !boxes.isEmpty())
            throw std::logic_error("Spatial queries require a query index");
        if (!nodes.isEmpty())
            query(0, box, work, visit);
    }

private:
    struct Node
    {
        Box box;
        int begin, end;
        int left = -1, right = -1;
        bool leaf() const { return left < 0; }
    };

    template <class Work> int build(int begin, int end, Work& work)
    {
        work();
        Box box = boxes[order[begin]];
        for (int i = begin + 1; i < end; ++i) {
            const auto& next = boxes[order[i]];
            box.left = std::min(box.left, next.left);
            box.right = std::max(box.right, next.right);
            box.top = std::min(box.top, next.top);
            box.bottom = std::max(box.bottom, next.bottom);
        }
        const int index = nodes.size();
        nodes.append({ box, begin, end });
        if (end - begin > 4) {
            const bool horizontal = box.right * .5 - box.left * .5 >= box.bottom * .5 - box.top * .5;
            const int middle = begin + (end - begin) / 2;
            std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end, [&](int a, int b) {
                const auto& one = boxes[a];
                const auto& two = boxes[b];
                const double first = horizontal ? one.left * .5 + one.right * .5 : one.top * .5 + one.bottom * .5;
                const double second = horizontal ? two.left * .5 + two.right * .5 : two.top * .5 + two.bottom * .5;
                return first < second || (first == second && a < b);
            });
            const int left = build(begin, middle, work);
            const int right = build(middle, end, work);
            nodes[index].left = left;
            nodes[index].right = right;
        } else
            std::sort(order.begin() + begin, order.begin() + end);
        return index;
    }

    template <class Work, class Visit> void query(int index, const Box& box, Work& work, Visit& visit) const
    {
        work();
        const auto& node = nodes[index];
        if (!node.box.overlaps(box))
            return;
        if (node.leaf()) {
            for (int i = node.begin; i < node.end; ++i) {
                work();
                if (boxes[order[i]].overlaps(box))
                    visit(order[i]);
            }
        } else {
            query(node.left, box, work, visit);
            query(node.right, box, work, visit);
        }
    }

    QVector<Box> boxes;
    QVector<int> order;
    QVector<Node> nodes;
};
}

#endif
