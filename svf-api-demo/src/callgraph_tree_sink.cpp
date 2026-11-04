#include "callgraph_tree_sink.h"

#include <ostream>
#include <string>
#include <vector>

TreeSink::TreeSink(std::ostream& os) : os_(os) {}

void TreeSink::beginEntry(const std::string& name) {
    os_ << "===== entry: " << name << " =====\n";
    spans_.clear();
    kids_.clear();
}

void TreeSink::span(const FuncInfo& info, unsigned id,
                    const std::optional<unsigned>& parent, bool indirect,
                    unsigned depth, bool cycle,
                    const std::optional<CallSiteLoc>& callLoc) {
    (void)id;  // spans arrive in pre-order, so the vector index is the id
    SpanRecord r;
    r.info = &info;
    r.hasParent = parent.has_value();
    r.parent = r.hasParent ? *parent : 0;
    r.indirect = indirect;
    r.depth = depth;
    r.cycle = cycle;
    r.callLoc = callLoc;
    spans_.push_back(std::move(r));
}

void TreeSink::endEntry(const std::string& name, unsigned spans,
                        size_t reachable) {
    // Group children by parent. Parents are always emitted before their
    // children (pre-order), so the record index doubles as the span id.
    kids_.assign(spans_.size(), {});
    for (unsigned i = 0; i < spans_.size(); ++i)
        if (spans_[i].hasParent) kids_[spans_[i].parent].push_back(i);
    if (!spans_.empty())
        renderNode(0, "", /*last=*/true, /*hasParent=*/spans_[0].hasParent);
    os_ << "===== end: " << name << " spans=" << spans
        << " reachable=" << reachable << " =====\n";
}

void TreeSink::renderNode(unsigned idx, const std::string& prefix, bool last,
                          bool hasParent) {
    const SpanRecord& r = spans_[idx];
    os_ << prefix;
    if (hasParent)
        os_ << (last ? "└── " : "├── ");
    os_ << r.info->function;
    if (r.indirect)
        os_ << " [indirect]";
    if (r.cycle)
        os_ << " [cycle]";
    if (r.callLoc)
        os_ << "  (" << r.callLoc->file << ':' << r.callLoc->line << ')';
    os_ << '\n';
    const std::vector<unsigned>& kids = kids_[idx];
    const std::string childPrefix = prefix + (last ? "    " : "│   ");
    for (size_t i = 0; i < kids.size(); ++i)
        renderNode(kids[i], childPrefix, i + 1 == kids.size(), true);
}
