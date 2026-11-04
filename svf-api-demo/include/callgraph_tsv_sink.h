#ifndef CALLGRAPH_TSV_SINK_H
#define CALLGRAPH_TSV_SINK_H

#include "callgraph_sink.h"

#include <iosfwd>
#include <optional>
#include <string>

/// Escape a string for embedding in a tab-separated field: the separators that
/// would break the line (tab, newline, CR) plus the escape character itself.
std::string tsvEscape(const std::string& in);

/// Write one span as a tab-separated line:
///   span_id  parent_span_id  kind  depth  cycle  function  mangled  file
///   start_line  end_line  call_file  call_line
/// Empty fields mean "absent" (no parent, no debug line info, no call-site
/// location).
void writeSpanLine(std::ostream& os,
                   const FuncInfo& info,
                   unsigned span_id,
                   const std::optional<unsigned>& parent_span_id,
                   const std::string& kind,
                   unsigned depth,
                   bool cycle,
                   const std::optional<CallSiteLoc>& call_loc);

/// Machine-readable layout: one tab-separated span per line, streamed.
class TsvSink final : public SpanSink {
public:
    explicit TsvSink(std::ostream& os);

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
    std::ostream& os_;
};

#endif // CALLGRAPH_TSV_SINK_H
