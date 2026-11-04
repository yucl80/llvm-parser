#include "callgraph_tsv_sink.h"

#include <charconv>
#include <ostream>

std::string tsvEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += static_cast<char>(c);
        }
    }
    return out;
}

/// Append a non-negative integer without iostream formatting overhead
/// (locale, num_put): std::to_chars is allocation- and locale-free.
static inline void appendNum(std::string& out, unsigned v) {
    char tmp[16];
    std::to_chars_result r = std::to_chars(tmp, tmp + sizeof(tmp), v);
    out.append(tmp, static_cast<size_t>(r.ptr - tmp));
}

/// Append a field, escaping the tab/newline/CR/backslash separators. The
/// common case (no specials) is a single find + append — no allocation; only
/// fields that actually contain a special character pay the escaping loop.
static inline void appendEscaped(std::string& out, const std::string& in) {
    if (in.find_first_of("\\\t\n\r") == std::string::npos) {
        out.append(in);
        return;
    }
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c);
        }
    }
}

void writeSpanLine(std::ostream& os,
                   const FuncInfo& info,
                   unsigned span_id,
                   const std::optional<unsigned>& parent_span_id,
                   const std::string& kind,
                   unsigned depth,
                   bool cycle,
                   const std::optional<CallSiteLoc>& call_loc) {
    // The line is assembled in a reusable buffer and written in one shot: the
    // per-span cost drops from ~11 ostream << calls (each a streambuf virtual
    // call) to a single os.write, and fields that need no escaping skip the
    // per-field string allocation. The thread-local buffer's capacity is
    // reused across spans, so the allocation cost amortizes to once. The
    // per-function strings come from the descriptor cache, so no span is ever
    // copied.
    thread_local std::string buf;
    buf.clear();
    appendNum(buf, span_id);
    buf.push_back('\t');
    if (parent_span_id) appendNum(buf, *parent_span_id);
    buf.push_back('\t');
    buf.append(kind);
    buf.push_back('\t');
    appendNum(buf, depth);
    buf.push_back('\t');
    buf.push_back(cycle ? '1' : '0');
    buf.push_back('\t');
    appendEscaped(buf, info.function);
    buf.push_back('\t');
    appendEscaped(buf, info.mangled);
    buf.push_back('\t');
    appendEscaped(buf, info.file);
    buf.push_back('\t');
    if (info.hasLines) appendNum(buf, info.start);
    buf.push_back('\t');
    if (info.hasLines) appendNum(buf, info.end);
    buf.push_back('\t');
    if (call_loc) appendEscaped(buf, call_loc->file);
    buf.push_back('\t');
    if (call_loc) appendNum(buf, call_loc->line);
    buf.push_back('\n');
    os.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

TsvSink::TsvSink(std::ostream& os) : os_(os) {}

void TsvSink::beginEntry(const std::string& name) {
    os_ << "#entry " << tsvEscape(name) << "\n";
}

void TsvSink::span(const FuncInfo& info, unsigned id,
                   const std::optional<unsigned>& parent, bool indirect,
                   unsigned depth, bool cycle,
                   const std::optional<CallSiteLoc>& callLoc) {
    (void)id;  // the span id is already the first field written by the writer
    writeSpanLine(os_, info, id, parent, indirect ? "indirect" : "direct",
                  depth, cycle, callLoc);
}

void TsvSink::endEntry(const std::string& name, unsigned spans,
                       size_t reachable) {
    os_ << "#end " << tsvEscape(name) << " spans=" << spans
        << " reachable_funcs=" << reachable << "\n";
}
