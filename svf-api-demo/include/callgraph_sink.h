#ifndef CALLGRAPH_SINK_H
#define CALLGRAPH_SINK_H

#include "callgraph_span.h"

#include <optional>
#include <string>

/// Traversal output interface. The traversal itself stays streaming: the TSV
/// sink writes each span the moment it is generated, while the tree sink
/// buffers one entry's spans — a node's `├──`/`└──` connector depends on
/// whether a later sibling will be emitted, which is only known after the
/// parent's out-edges have all been walked — and renders the connectors when
/// the entry completes.
class SpanSink {
public:
    virtual ~SpanSink() = default;
    virtual void beginEntry(const std::string& name) = 0;
    virtual void span(const FuncInfo& info,
                      unsigned id,
                      const std::optional<unsigned>& parent,
                      bool indirect,
                      unsigned depth,
                      bool cycle,
                      const std::optional<CallSiteLoc>& callLoc) = 0;
    virtual void endEntry(const std::string& name,
                          unsigned spans,
                          size_t reachable) = 0;
};

#endif // CALLGRAPH_SINK_H
