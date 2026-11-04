#ifndef CALLGRAPH_TREE_SINK_H
#define CALLGRAPH_TREE_SINK_H

#include "callgraph_sink.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

/// One span produced by the traversal, as stored by the tree renderer.
/// Strings are not copied: \p info points into the FuncInfoCache, whose
/// elements are stable for the whole report.
struct SpanRecord {
    const FuncInfo* info;
    unsigned parent = 0;
    bool hasParent = false;
    bool indirect = false;
    unsigned depth = 0;
    bool cycle = false;
    std::optional<CallSiteLoc> callLoc;
};

/// Human-readable layout: each entry's call chain as an indented ASCII tree.
/// Cell width is 4 columns (`│   `/`    ` for ancestors, `├── `/`└── ` for the
/// node itself); the depth-1 level is indented because the root contributes a
/// cell too, mirroring the topology examples in the README.
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
    void renderNode(unsigned idx, const std::string& prefix, bool last,
                    bool hasParent);

    std::ostream& os_;
    std::vector<SpanRecord> spans_;
    std::vector<std::vector<unsigned>> kids_;
};

#endif // CALLGRAPH_TREE_SINK_H
