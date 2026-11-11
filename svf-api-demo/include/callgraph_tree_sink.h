#ifndef CALLGRAPH_TREE_SINK_H
#define CALLGRAPH_TREE_SINK_H

#include "callgraph_sink.h"

#include <cstddef>
#include <iosfwd>
#include <limits>
#include <optional>
#include <string>
#include <vector>

/// One span produced by the traversal, as stored by the tree renderer.
/// The pointers refer to the FuncInfo and call-site caches, whose elements
/// remain stable for the whole report.
struct SpanRecord {
    const FuncInfo* info;
    unsigned parent = 0;
    bool hasParent = false;
    bool indirect = false;
    unsigned depth = 0;
    bool cycle = false;
    const CallSiteLoc* callLoc = nullptr;
};

/// Human-readable layout: each entry's call chain as an indented ASCII tree.
/// Every span is labelled with its call mode (`[root]`, `[direct]` or
/// `[indirect]`, plus `[cycle]` on back-edges), mirroring the TSV `kind`
/// column. Cell width is 4 columns (`│   `/`    ` for ancestors, `├── `/`└── `
/// for the node itself); the depth-1 level is indented because the root
/// contributes a cell too, mirroring the topology examples in the README.
class TreeSink final : public SpanSink {
public:
    explicit TreeSink(std::ostream& os);

    void beginEntry(const std::string& name) override;
    void span(const FuncInfo& info,
              unsigned id,
              const std::optional<unsigned>& parent,
              bool indirect,
              unsigned depth,
              bool cycle,
              const std::optional<CallSiteLoc>& callLoc) override;
    void endEntry(const std::string& name,
                  unsigned spans,
                  size_t reachable) override;

private:
    static constexpr size_t kNoSpan = std::numeric_limits<size_t>::max();

    void renderNode(size_t idx, const std::string& prefix, bool last,
                    bool hasParent);

    std::ostream& os_;
    std::vector<SpanRecord> spans_;
    std::vector<size_t> firstChild_;
    std::vector<size_t> lastChild_;
    std::vector<size_t> nextSibling_;
};

#endif // CALLGRAPH_TREE_SINK_H
