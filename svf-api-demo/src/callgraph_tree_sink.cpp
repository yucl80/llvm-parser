#include "callgraph_tree_sink.h"

#include <charconv>
#include <ostream>
#include <string>
#include <vector>

/// Append a non-negative integer without iostream formatting overhead,
/// mirroring the TSV sink's writer.
static inline void appendNum(std::string& out, unsigned v) {
    char tmp[16];
    std::to_chars_result r = std::to_chars(tmp, tmp + sizeof(tmp), v);
    out.append(tmp, static_cast<size_t>(r.ptr - tmp));
}

TreeSink::TreeSink(std::ostream& os) : os_(os) {}

void TreeSink::beginEntry(const std::string& name) {
    os_ << "===== entry: " << name << " =====\n";
    spans_.clear();
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
    if (!spans_.empty())
        renderAll();
    os_ << "===== end: " << name << " spans=" << spans
        << " reachable=" << reachable << " =====\n";
}

void TreeSink::renderAll() {
    // Spans arrive in pre-order, so a node's subtree is a contiguous range and
    // its depth is the visible depth (hidden nodes do not bump it). Every
    // emitted child has depth = parent.depth + 1, even through hidden nodes,
    // so a reverse pass keyed by depth finds the last child of each parent.
    // This replaces the span-indexed `lastOfParent` table (8 bytes/span) with
    // an O(maxDepth) array; the flag itself lives in SpanRecord's padding.
    std::vector<size_t> lastAtDepth;
    for (size_t i = spans_.size(); i-- > 0;) {
        SpanRecord& r = spans_[i];
        if (!r.hasParent) continue;
        if (r.depth >= lastAtDepth.size())
            lastAtDepth.resize(r.depth + 1, kNoSpan);
        size_t& last = lastAtDepth[r.depth];
        if (last == kNoSpan) {
            r.isLast = true;
            last = i;
        }
    }
    // The recursive renderer passed last=true for the root, giving its children
    // blank indent cells; preserve that here.
    if (!spans_.empty()) spans_[0].isLast = true;

    std::string prefix;
    // Prefix length before each level's cell was appended. A cell is 4 bytes
    // ("    ") or 6 bytes ("│   ", where │ is a 3-byte UTF-8 sequence), so
    // unwinding must restore the recorded length rather than assume a width.
    std::vector<size_t> levelPrefixLens;
    thread_local std::string buf;

    for (size_t i = 0; i < spans_.size(); ++i) {
        const SpanRecord& r = spans_[i];
        // A second root (a hidden chain grafted under no span) was never
        // reached by the recursive renderer; keep that behaviour.
        if (i != 0 && !r.hasParent) break;

        // Pre-order: depth grows by at most one, or unwinds to an ancestor.
        while (levelPrefixLens.size() > r.depth) {
            prefix.resize(levelPrefixLens.back());
            levelPrefixLens.pop_back();
        }

        // Assemble the whole line in a reusable buffer and issue a single
        // ostream::write instead of ~10 `<<` calls per span.
        buf.clear();
        buf.append(prefix);
        if (r.hasParent)
            buf += (r.isLast ? "└── " : "├── ");
        buf += r.info->function;
        // Label the call mode of every span, mirroring the TSV `kind` column.
        // Root spans have no incoming edge; the rest are direct or indirect
        // call edges. Only indirect calls used to carry a marker, which left the
        // mode of ordinary calls invisible in the tree layout.
        if (!r.hasParent)
            buf += " [root]";
        else
            buf += (r.indirect ? " [indirect]" : " [direct]");
        if (r.cycle)
            buf += " [cycle]";
        if (r.callLoc) {
            buf += "  (";
            buf += r.callLoc->file;
            buf += ':';
            appendNum(buf, r.callLoc->line);
            buf += ')';
        }
        buf += '\n';
        os_.write(buf.data(), static_cast<std::streamsize>(buf.size()));

        // Push a cell for possible children; the next sibling or ancestor
        // unwinds it above.
        levelPrefixLens.push_back(prefix.size());
        prefix += (r.isLast ? "    " : "│   ");
    }
}
