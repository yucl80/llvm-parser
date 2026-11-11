#include "callgraph_tree_sink.h"

#include <ostream>
#include <string>
#include <vector>

TreeSink::TreeSink(std::ostream& os) : os_(os) {}

void TreeSink::beginEntry(const std::string& name) {
    os_ << "===== entry: " << name << " =====\n";
    spans_.clear();
    firstChild_.clear();
    lastChild_.clear();
    nextSibling_.clear();
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
    r.callLoc = callLoc ? &*callLoc : nullptr;
    spans_.push_back(std::move(r));
}

void TreeSink::endEntry(const std::string& name, unsigned spans,
                        size_t reachable) {
    // Group children by parent. Parents are always emitted before their
    // children (pre-order), so the record index doubles as the span id.
    firstChild_.assign(spans_.size(), kNoSpan);
    lastChild_.assign(spans_.size(), kNoSpan);
    nextSibling_.assign(spans_.size(), kNoSpan);
    for (size_t i = 0; i < spans_.size(); ++i) {
        if (!spans_[i].hasParent) continue;
        const size_t parent = spans_[i].parent;
        if (firstChild_[parent] == kNoSpan)
            firstChild_[parent] = i;
        else
            nextSibling_[lastChild_[parent]] = i;
        lastChild_[parent] = i;
    }
    if (!spans_.empty())
        renderNode(0, "", /*last=*/true, /*hasParent=*/spans_[0].hasParent);
    os_ << "===== end: " << name << " spans=" << spans
        << " reachable=" << reachable << " =====\n";
}

void TreeSink::renderNode(size_t idx, const std::string& prefix, bool last,
                          bool hasParent) {
    const SpanRecord& r = spans_[idx];
    os_ << prefix;
    if (hasParent)
        os_ << (last ? "└── " : "├── ");
    os_ << r.info->function;
    // Label the call mode of every span, mirroring the TSV `kind` column.
    // Root spans have no incoming edge; the rest are direct or indirect
    // call edges. Only indirect calls used to carry a marker, which left the
    // mode of ordinary calls invisible in the tree layout.
    if (!r.hasParent)
        os_ << " [root]";
    else
        os_ << (r.indirect ? " [indirect]" : " [direct]");
    if (r.cycle)
        os_ << " [cycle]";
    if (r.callLoc)
        os_ << "  (" << r.callLoc->file << ':' << r.callLoc->line << ')';
    os_ << '\n';
    const std::string childPrefix = prefix + (last ? "    " : "│   ");
    for (size_t child = firstChild_[idx]; child != kNoSpan;
         child = nextSibling_[child])
        renderNode(child, childPrefix, child == lastChild_[idx], true);
}
