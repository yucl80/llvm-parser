#include <clang-c/Index.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct Location {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
};

struct CallSite {
    std::string calleeKey;
    std::string calleeVariableKey;
    std::string calleeName;
    std::string networkApi;
    Location location;
    std::vector<Location> loops;
    std::vector<std::string> callbackTargets;
    std::vector<std::vector<std::string>> argumentTargets;
    std::vector<std::vector<std::string>> argumentVariables;
    std::vector<std::string> parameterKeys;
};

struct FunctionInfo {
    std::string key;
    std::string name;
    std::vector<CallSite> calls;
};

struct Finding {
    std::string function;
    std::string call;
    std::string networkApi;
    std::vector<std::string> chain;
    Location loopLocation;
    Location callLocation;
};

struct Options {
    std::string format = "text";
    std::string output;
    std::vector<std::string> inputs;
    std::vector<std::string> clangArgs;
    std::vector<std::string> extraApis;
};

void collectDefinitions(
    CXCursor cursor,
    std::unordered_map<std::string, CXCursor>& definitions);
void collectFunctionPointerTargets(
    CXCursor cursor,
    std::unordered_map<std::string, std::unordered_set<std::string>>& targets,
    std::unordered_map<std::string, std::unordered_set<std::string>>& aliases,
    std::unordered_map<std::string, std::string>& networkApiTargets,
    const std::vector<std::string>& extraApis);

std::string toStd(CXString value) {
    const char* text = clang_getCString(value);
    std::string result = text ? text : "";
    clang_disposeString(value);
    return result;
}

std::vector<CXCursor> children(CXCursor cursor) {
    std::vector<CXCursor> result;
    clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor, CXClientData data) {
            static_cast<std::vector<CXCursor>*>(data)->push_back(child);
            return CXChildVisit_Continue;
        },
        &result);
    return result;
}

bool isFunctionKind(CXCursorKind kind) {
    return kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
           kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
           kind == CXCursor_ConversionFunction ||
           kind == CXCursor_FunctionTemplate;
}

Location sourceLocation(CXCursor cursor) {
    CXFile file = nullptr;
    Location result;
    clang_getExpansionLocation(clang_getCursorLocation(cursor), &file,
                               &result.line, &result.column, nullptr);
    if (file)
        result.file = toStd(clang_getFileName(file));
    return result;
}

std::string functionKey(CXCursor cursor) {
    std::string key = toStd(clang_getCursorUSR(cursor));
    if (key.empty()) {
        const Location location = sourceLocation(cursor);
        key = location.file + ":" + std::to_string(location.line) + ":" +
              std::to_string(location.column) + ":" +
              toStd(clang_getCursorSpelling(cursor));
    }
    return key;
}

std::string variableKey(CXCursor cursor) {
    std::string key = toStd(clang_getCursorUSR(cursor));
    if (key.empty()) {
        const Location location = sourceLocation(cursor);
        key = location.file + ":" + std::to_string(location.line) + ":" +
              std::to_string(location.column) + ":" +
              toStd(clang_getCursorSpelling(cursor));
    }
    return key;
}

std::string qualifiedName(CXCursor cursor) {
    std::vector<std::string> parts;
    for (CXCursor parent = clang_getCursorSemanticParent(cursor);
         !clang_Cursor_isNull(parent);
         parent = clang_getCursorSemanticParent(parent)) {
        const CXCursorKind kind = clang_getCursorKind(parent);
        if (kind != CXCursor_Namespace && kind != CXCursor_ClassDecl &&
            kind != CXCursor_StructDecl && kind != CXCursor_UnionDecl &&
            kind != CXCursor_ClassTemplate &&
            kind != CXCursor_ClassTemplatePartialSpecialization)
            break;
        const std::string part = toStd(clang_getCursorSpelling(parent));
        if (!part.empty())
            parts.push_back(part);
    }
    std::string result;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        result += *it + "::";
    return result + toStd(clang_getCursorSpelling(cursor));
}

CXCursor findFunctionReference(CXCursor cursor, unsigned depth = 0) {
    if (depth > 8 || clang_Cursor_isNull(cursor))
        return clang_getNullCursor();
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr ||
        kind == CXCursor_MemberRef) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (!clang_Cursor_isNull(referenced) &&
            isFunctionKind(clang_getCursorKind(referenced)))
            return referenced;
    }
    for (CXCursor child : children(cursor)) {
        CXCursor found = findFunctionReference(child, depth + 1);
        if (!clang_Cursor_isNull(found))
            return found;
    }
    return clang_getNullCursor();
}

CXCursor findVariableReference(CXCursor cursor, unsigned depth = 0) {
    if (depth > 12 || clang_Cursor_isNull(cursor))
        return clang_getNullCursor();
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr ||
        kind == CXCursor_MemberRef) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        const CXCursorKind referencedKind = clang_getCursorKind(referenced);
        if (referencedKind == CXCursor_VarDecl ||
            referencedKind == CXCursor_ParmDecl)
            return referenced;
    }
    for (CXCursor child : children(cursor)) {
        CXCursor found = findVariableReference(child, depth + 1);
        if (!clang_Cursor_isNull(found))
            return found;
    }
    return clang_getNullCursor();
}

bool isNetworkApi(const std::string& name,
                  const std::vector<std::string>& extraApis);
bool isNetworkApiCursor(const std::string& name, CXCursor callee,
                        const std::vector<std::string>& extraApis);
bool containsLambda(CXCursor cursor, unsigned depth = 0);

void collectLambdaTargets(CXCursor cursor, std::vector<std::string>& targets,
                          unsigned depth = 0) {
    if (depth > 12 || clang_Cursor_isNull(cursor))
        return;
    if (clang_getCursorKind(cursor) == CXCursor_LambdaExpr) {
        for (CXCursor child : children(cursor)) {
            if (clang_getCursorKind(child) == CXCursor_CXXMethod &&
                toStd(clang_getCursorSpelling(child)) == "operator()") {
                targets.push_back(functionKey(child));
                return;
            }
        }
    }
    for (CXCursor child : children(cursor))
        collectLambdaTargets(child, targets, depth + 1);
}

CXCursor findLambdaBody(CXCursor cursor, unsigned depth = 0) {
    if (depth > 12 || clang_Cursor_isNull(cursor))
        return clang_getNullCursor();
    if (clang_getCursorKind(cursor) == CXCursor_CompoundStmt)
        return cursor;
    for (CXCursor child : children(cursor)) {
        CXCursor body = findLambdaBody(child, depth + 1);
        if (!clang_Cursor_isNull(body))
            return body;
    }
    return clang_getNullCursor();
}

void collectLambdaNetworkApis(
    CXCursor cursor, std::vector<std::string>& targets,
    std::unordered_map<std::string, std::string>& networkApiTargets,
    const std::vector<std::string>& extraApis, unsigned depth = 0) {
    if (depth > 20 || clang_Cursor_isNull(cursor))
        return;
    if (clang_getCursorKind(cursor) == CXCursor_LambdaExpr) {
        CXCursor body = findLambdaBody(cursor);
        if (clang_Cursor_isNull(body))
            return;
        std::vector<CXCursor> pending{body};
        while (!pending.empty()) {
            CXCursor current = pending.back();
            pending.pop_back();
            if (clang_getCursorKind(current) == CXCursor_LambdaExpr) {
                collectLambdaNetworkApis(current, targets, networkApiTargets,
                                         extraApis, depth + 1);
                continue;
            }
            if (clang_getCursorKind(current) == CXCursor_CallExpr) {
                const std::vector<CXCursor> callChildren = children(current);
                if (!callChildren.empty()) {
                    CXCursor function =
                        findFunctionReference(callChildren.front());
                    if (!clang_Cursor_isNull(function)) {
                        const std::string name = qualifiedName(function);
                        if (isNetworkApiCursor(name, function, extraApis)) {
                            const Location location = sourceLocation(cursor);
                            const std::string key =
                                "lambda:" + location.file + ":" +
                                std::to_string(location.line) + ":" +
                                std::to_string(location.column) + ":" + name;
                            targets.push_back(key);
                            networkApiTargets[key] = name;
                        }
                    }
                }
            }
            const std::vector<CXCursor> currentChildren = children(current);
            pending.insert(pending.end(), currentChildren.begin(),
                           currentChildren.end());
        }
        return;
    }
    for (CXCursor child : children(cursor))
        collectLambdaNetworkApis(child, targets, networkApiTargets, extraApis,
                                 depth + 1);
}

void collectCallableValues(CXCursor expression,
                           std::vector<std::string>& functionTargets,
                           std::vector<std::string>& variableTargets,
                           std::unordered_map<std::string, std::string>&
                               networkApiTargets,
                           const std::vector<std::string>& extraApis) {
    collectLambdaTargets(expression, functionTargets);
    if (containsLambda(expression)) {
        collectLambdaNetworkApis(expression, functionTargets,
                                 networkApiTargets, extraApis);
        return;
    }
    CXCursor function = findFunctionReference(expression);
    if (!clang_Cursor_isNull(function)) {
        const std::string key = functionKey(function);
        const std::string name = qualifiedName(function);
        functionTargets.push_back(key);
        if (isNetworkApiCursor(name, function, extraApis))
            networkApiTargets[key] = name;
        return;
    }
    CXCursor variable = findVariableReference(expression);
    if (!clang_Cursor_isNull(variable))
        variableTargets.push_back(variableKey(variable));
}

bool isAssignment(CXCursor cursor) {
    CXTranslationUnit unit = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned tokenCount = 0;
    clang_tokenize(unit, clang_getCursorExtent(cursor), &tokens, &tokenCount);
    bool assignment = false;
    for (unsigned i = 0; i < tokenCount; ++i) {
        if (toStd(clang_getTokenSpelling(unit, tokens[i])) == "=") {
            assignment = true;
            break;
        }
    }
    clang_disposeTokens(unit, tokens, tokenCount);
    return assignment;
}

bool isSystemHeader(CXCursor cursor);

void collectFunctionPointerTargets(
    CXCursor cursor,
    std::unordered_map<std::string, std::unordered_set<std::string>>& targets,
    std::unordered_map<std::string, std::unordered_set<std::string>>& aliases,
    std::unordered_map<std::string, std::string>& networkApiTargets,
    const std::vector<std::string>& extraApis);

bool isFunctionPointerType(CXCursor cursor) {
    CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
    if (type.kind == CXType_Pointer || type.kind == CXType_BlockPointer) {
        type = clang_getCanonicalType(clang_getPointeeType(type));
        return type.kind == CXType_FunctionProto ||
               type.kind == CXType_FunctionNoProto;
    }
    return false;
}

bool containsLambda(CXCursor cursor, unsigned depth) {
    if (depth > 12 || clang_Cursor_isNull(cursor))
        return false;
    if (clang_getCursorKind(cursor) == CXCursor_LambdaExpr)
        return true;
    for (CXCursor child : children(cursor)) {
        if (containsLambda(child, depth + 1))
            return true;
    }
    return false;
}

void collectFunctionPointerTargets(
    CXCursor cursor,
    std::unordered_map<std::string, std::unordered_set<std::string>>& targets,
    std::unordered_map<std::string, std::unordered_set<std::string>>& aliases,
    std::unordered_map<std::string, std::string>& networkApiTargets,
    const std::vector<std::string>& extraApis) {
    if (isSystemHeader(cursor))
        return;

    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_VarDecl &&
        (isFunctionPointerType(cursor) || containsLambda(cursor))) {
        std::vector<std::string> functionTargets;
        std::vector<std::string> variableTargets;
        for (CXCursor child : children(cursor))
            collectCallableValues(child, functionTargets, variableTargets,
                                  networkApiTargets, extraApis);
        const std::string key = variableKey(cursor);
        targets[key].insert(functionTargets.begin(), functionTargets.end());
        aliases[key].insert(variableTargets.begin(), variableTargets.end());
    } else if (kind == CXCursor_BinaryOperator && isAssignment(cursor)) {
        const std::vector<CXCursor> operands = children(cursor);
        if (operands.size() >= 2) {
            CXCursor variable = findVariableReference(operands.front());
            if (!clang_Cursor_isNull(variable) &&
                isFunctionPointerType(variable)) {
                std::vector<std::string> functionTargets;
                std::vector<std::string> variableTargets;
                collectCallableValues(operands.back(), functionTargets,
                                      variableTargets, networkApiTargets,
                                      extraApis);
                const std::string key = variableKey(variable);
                targets[key].insert(functionTargets.begin(),
                                    functionTargets.end());
                aliases[key].insert(variableTargets.begin(),
                                    variableTargets.end());
            }
        }
    }

    for (CXCursor child : children(cursor))
        collectFunctionPointerTargets(child, targets, aliases,
                                      networkApiTargets, extraApis);
}

std::string unqualified(const std::string& name) {
    const std::size_t separator = name.rfind("::");
    return separator == std::string::npos ? name : name.substr(separator + 2);
}

bool contains(const std::vector<std::string>& values,
              const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

const std::vector<std::string>& builtinApis() {
    static const std::vector<std::string> apis = {
        "curl_easy_perform", "curl_multi_perform", "curl_easy_send",
        "curl_easy_recv", "curl_multi_socket_action",
        "connect", "connectx", "WSAConnect", "getaddrinfo",
        "gethostbyname", "gethostbyname2", "send", "sendto", "sendmsg",
        "WSASend", "recv", "recvfrom", "recvmsg", "WSARecv",
        "SSL_connect", "SSL_write", "SSL_read",
        "WinHttpSendRequest", "WinHttpReceiveResponse",
        "HttpSendRequestA", "HttpSendRequestW",
        "InternetOpenUrlA", "InternetOpenUrlW"};
    return apis;
}

bool isNetworkApi(const std::string& name,
                  const std::vector<std::string>& extraApis) {
    const std::string shortName = unqualified(name);
    if (contains(extraApis, name) || contains(extraApis, shortName))
        return true;
    const bool hasNamespace = name.find("::") != std::string::npos;
    if (!hasNamespace && contains(builtinApis(), shortName))
        return true;

    const bool httpVerb =
        shortName == "Get" || shortName == "Post" || shortName == "Put" ||
        shortName == "Patch" || shortName == "Delete" ||
        shortName == "Head" || shortName == "Options";
    if (httpVerb &&
        (name.rfind("httplib::", 0) == 0 || name.rfind("cpr::", 0) == 0))
        return true;
    if (name.rfind("Poco::Net::HTTPClientSession::", 0) == 0 ||
        name.rfind("Poco::Net::HTTPSClientSession::", 0) == 0)
        return shortName == "sendRequest" || shortName == "receiveResponse";
    if (name.rfind("boost::beast::http::", 0) == 0 ||
        name.rfind("boost::asio::", 0) == 0 || name.rfind("asio::", 0) == 0)
        return shortName == "connect" || shortName == "async_connect" ||
               shortName == "write" || shortName == "async_write" ||
               shortName == "read" || shortName == "async_read" ||
               shortName == "request";
    return false;
}

bool isSystemHeader(CXCursor cursor) {
    return clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)) != 0;
}

bool isNetworkApiCursor(const std::string& name, CXCursor callee,
                        const std::vector<std::string>& extraApis) {
    const std::string shortName = unqualified(name);
    if (contains(extraApis, name) || contains(extraApis, shortName))
        return true;
    if (!isNetworkApi(name, extraApis))
        return false;
    if (name.find("::") == std::string::npos &&
        contains(builtinApis(), shortName))
        return isSystemHeader(callee);
    return true;
}

void collectFunctions(CXCursor cursor,
                      std::unordered_map<std::string, FunctionInfo>& functions) {
    if (isFunctionKind(clang_getCursorKind(cursor)) &&
        clang_isCursorDefinition(cursor) && !isSystemHeader(cursor)) {
        FunctionInfo function;
        function.key = functionKey(cursor);
        function.name = qualifiedName(cursor);
        functions.emplace(function.key, std::move(function));
    }
    for (CXCursor child : children(cursor))
        collectFunctions(child, functions);
}

enum class StaticTruth {
    Unknown,
    True,
    False,
};

std::string tokenText(CXCursor cursor) {
    CXTranslationUnit unit = clang_Cursor_getTranslationUnit(cursor);
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXToken* tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(unit, range, &tokens, &count);
    std::string result;
    for (unsigned i = 0; i < count; ++i) {
        if (!result.empty())
            result += " ";
        result += toStd(clang_getTokenSpelling(unit, tokens[i]));
    }
    clang_disposeTokens(unit, tokens, count);
    return result;
}

StaticTruth evaluateStaticTruth(CXCursor cursor) {
    if (clang_Cursor_isNull(cursor))
        return StaticTruth::Unknown;
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_CXXBoolLiteralExpr) {
        const std::string text = tokenText(cursor);
        if (text == "true")
            return StaticTruth::True;
        if (text == "false")
            return StaticTruth::False;
    }
    if (kind == CXCursor_IntegerLiteral) {
        long long value = 0;
        const std::string text = tokenText(cursor);
        char* end = nullptr;
        value = std::strtoll(text.c_str(), &end, 0);
        if (end != text.c_str() && *end == '\0')
            return value != 0 ? StaticTruth::True : StaticTruth::False;
    }
    if (kind == CXCursor_CharacterLiteral) {
        const std::string text = tokenText(cursor);
        return text.size() >= 3 ? StaticTruth::True : StaticTruth::False;
    }
    const std::vector<CXCursor> cursorChildren = children(cursor);
    if (kind == CXCursor_UnaryOperator && !cursorChildren.empty()) {
        const std::string text = tokenText(cursor);
        if (text.find('!') == 0) {
            const StaticTruth child = evaluateStaticTruth(cursorChildren.front());
            if (child == StaticTruth::True)
                return StaticTruth::False;
            if (child == StaticTruth::False)
                return StaticTruth::True;
        }
    }
    if (kind == CXCursor_BinaryOperator && cursorChildren.size() >= 2) {
        const std::string text = tokenText(cursor);
        const CXCursor left = cursorChildren[0];
        const CXCursor right = cursorChildren[1];
        const StaticTruth leftTruth = evaluateStaticTruth(left);
        const StaticTruth rightTruth = evaluateStaticTruth(right);
        if (text.find("&&") != std::string::npos) {
            if (leftTruth == StaticTruth::False ||
                rightTruth == StaticTruth::False)
                return StaticTruth::False;
            if (leftTruth == StaticTruth::True &&
                rightTruth == StaticTruth::True)
                return StaticTruth::True;
        }
        if (text.find("||") != std::string::npos) {
            if (leftTruth == StaticTruth::True ||
                rightTruth == StaticTruth::True)
                return StaticTruth::True;
            if (leftTruth == StaticTruth::False &&
                rightTruth == StaticTruth::False)
                return StaticTruth::False;
        }
    }
    if (!cursorChildren.empty()) {
        if (kind == CXCursor_ParenExpr ||
            kind == CXCursor_CStyleCastExpr ||
            kind == CXCursor_CXXStaticCastExpr ||
            kind == CXCursor_CXXFunctionalCastExpr)
            return evaluateStaticTruth(cursorChildren.front());
    }
    return StaticTruth::Unknown;
}

bool evaluateInteger(CXCursor cursor, long long& value) {
    if (clang_Cursor_isNull(cursor))
        return false;
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_IntegerLiteral ||
        kind == CXCursor_CXXBoolLiteralExpr) {
        const std::string text = tokenText(cursor);
        char* end = nullptr;
        value = std::strtoll(text.c_str(), &end, 0);
        if (text == "true")
            value = 1;
        else if (text == "false")
            value = 0;
        else if (end == text.c_str() || *end != '\0')
            return false;
        return true;
    }
    const std::vector<CXCursor> cursorChildren = children(cursor);
    if (!cursorChildren.empty() &&
        (kind == CXCursor_ParenExpr ||
         kind == CXCursor_CStyleCastExpr ||
         kind == CXCursor_CXXStaticCastExpr ||
        kind == CXCursor_CXXFunctionalCastExpr))
        return evaluateInteger(cursorChildren.front(), value);
    return false;
}

StaticTruth evaluateDeclaredForCondition(
    const std::vector<CXCursor>& loopChildren, CXCursor condition) {
    if (loopChildren.empty())
        return StaticTruth::Unknown;
    if (clang_getCursorKind(loopChildren.front()) != CXCursor_DeclStmt)
        return StaticTruth::Unknown;

    std::string variableName;
    long long initialValue = 0;
    bool hasInitialValue = false;
    for (CXCursor declaration : children(loopChildren.front())) {
        if (clang_getCursorKind(declaration) != CXCursor_VarDecl)
            continue;
        variableName = toStd(clang_getCursorSpelling(declaration));
        for (CXCursor initializer : children(declaration)) {
            if (evaluateInteger(initializer, initialValue)) {
                hasInitialValue = true;
                break;
            }
        }
    }
    if (!hasInitialValue || variableName.empty())
        return StaticTruth::Unknown;

    const std::vector<CXCursor> operands = children(condition);
    if (clang_getCursorKind(condition) != CXCursor_BinaryOperator ||
        operands.size() < 2)
        return StaticTruth::Unknown;
    if (clang_getCursorKind(operands.front()) != CXCursor_DeclRefExpr &&
        clang_getCursorKind(operands.front()) != CXCursor_UnexposedExpr)
        return StaticTruth::Unknown;
    if (toStd(clang_getCursorSpelling(operands.front())) != variableName)
        return StaticTruth::Unknown;

    long long right = 0;
    if (!evaluateInteger(operands[1], right))
        return StaticTruth::Unknown;
    const std::string text = tokenText(condition);
    if (text.find("<=") != std::string::npos)
        return initialValue <= right ? StaticTruth::True : StaticTruth::False;
    if (text.find(">=") != std::string::npos)
        return initialValue >= right ? StaticTruth::True : StaticTruth::False;
    if (text.find('<') != std::string::npos &&
        text.find("<<") == std::string::npos)
        return initialValue < right ? StaticTruth::True : StaticTruth::False;
    if (text.find('>') != std::string::npos &&
        text.find(">>") == std::string::npos)
        return initialValue > right ? StaticTruth::True : StaticTruth::False;
    if (text.find("==") != std::string::npos)
        return initialValue == right ? StaticTruth::True : StaticTruth::False;
    if (text.find("!=") != std::string::npos)
        return initialValue != right ? StaticTruth::True : StaticTruth::False;
    return StaticTruth::Unknown;
}

struct WalkState {
    FunctionInfo* function;
    const std::vector<std::string>* extraApis;
    std::unordered_map<std::string, std::string>* networkApiTargets;
    std::vector<Location> activeLoops;
};

void collectCalls(CXCursor cursor, WalkState& state, bool root = false) {
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (!root && (isFunctionKind(kind) || kind == CXCursor_LambdaExpr))
        return;

    if (kind == CXCursor_CompoundStmt) {
        for (CXCursor child : children(cursor)) {
            const CXCursorKind childKind = clang_getCursorKind(child);
            collectCalls(child, state);
            if (childKind == CXCursor_ReturnStmt ||
                childKind == CXCursor_BreakStmt ||
                childKind == CXCursor_ContinueStmt ||
                childKind == CXCursor_GotoStmt)
                break;
        }
        return;
    }

    if (kind == CXCursor_IfStmt) {
        const std::vector<CXCursor> ifChildren = children(cursor);
        if (!ifChildren.empty()) {
            const StaticTruth condition =
                evaluateStaticTruth(ifChildren.front());
            if (condition == StaticTruth::False) {
                if (ifChildren.size() >= 3)
                    collectCalls(ifChildren[2], state);
                return;
            }
            if (condition == StaticTruth::True) {
                if (ifChildren.size() >= 2)
                    collectCalls(ifChildren[1], state);
                return;
            }
        }
    }

    if (kind == CXCursor_ForStmt || kind == CXCursor_WhileStmt ||
        kind == CXCursor_DoStmt || kind == CXCursor_CXXForRangeStmt) {
        const std::vector<CXCursor> loopChildren = children(cursor);
        if (loopChildren.empty())
            return;
        const CXCursor body =
            kind == CXCursor_DoStmt ? loopChildren.front() : loopChildren.back();
        if (kind == CXCursor_WhileStmt && loopChildren.size() >= 2) {
            const StaticTruth condition =
                evaluateStaticTruth(loopChildren.front());
            if (condition == StaticTruth::False)
                return;
        }
        if (kind == CXCursor_ForStmt && loopChildren.size() >= 3) {
            const std::size_t conditionIndex =
                clang_getCursorKind(loopChildren.front()) ==
                        CXCursor_DeclStmt
                    ? 1
                    : 0;
            if (conditionIndex < loopChildren.size()) {
                CXCursor conditionCursor = loopChildren[conditionIndex];
                StaticTruth conditionTruth =
                    evaluateStaticTruth(conditionCursor);
                if (conditionTruth == StaticTruth::Unknown)
                    conditionTruth =
                        evaluateDeclaredForCondition(loopChildren, conditionCursor);
                if (conditionTruth == StaticTruth::False)
                    return;
            }
        }
        state.activeLoops.push_back(sourceLocation(cursor));
        if (kind == CXCursor_WhileStmt || kind == CXCursor_DoStmt) {
            for (CXCursor child : loopChildren) {
                if (!clang_equalCursors(child, body))
                    collectCalls(child, state);
            }
        } else if (kind == CXCursor_ForStmt) {
            std::size_t firstHeaderChild = 0;
            if (!loopChildren.empty() &&
                clang_getCursorKind(loopChildren.front()) ==
                    CXCursor_DeclStmt)
                firstHeaderChild = 1;
            for (std::size_t i = firstHeaderChild;
                 i + 1 < loopChildren.size(); ++i)
                collectCalls(loopChildren[i], state);
        }
        collectCalls(body, state);
        state.activeLoops.pop_back();
        return;
    }

    if (kind == CXCursor_CallExpr) {
        const std::vector<CXCursor> callChildren = children(cursor);
        CXCursor callee = callChildren.empty()
                              ? clang_getNullCursor()
                              : findFunctionReference(callChildren.front());
        CallSite call;
        call.location = sourceLocation(cursor);
        if (!clang_Cursor_isNull(callee)) {
            call.calleeKey = functionKey(callee);
            call.calleeName = qualifiedName(callee);
            if (isNetworkApiCursor(call.calleeName, callee,
                                   *state.extraApis)) {
                call.networkApi = call.calleeName;
                (*state.networkApiTargets)[call.calleeKey] = call.networkApi;
            }
            for (CXCursor child : children(callee)) {
                if (clang_getCursorKind(child) == CXCursor_ParmDecl) {
                    call.parameterKeys.push_back(
                        isFunctionPointerType(child) ? variableKey(child) : "");
                }
            }
        } else if (!callChildren.empty()) {
            CXCursor calleeVariable =
                findVariableReference(callChildren.front());
            if (!clang_Cursor_isNull(calleeVariable)) {
                call.calleeVariableKey = variableKey(calleeVariable);
                call.calleeName =
                    toStd(clang_getCursorSpelling(calleeVariable));
            }
        }
        if (clang_Cursor_isNull(callee) && !callChildren.empty() &&
            containsLambda(callChildren.front())) {
            std::vector<std::string> lambdaTargets;
            std::vector<std::string> variableTargets;
            collectCallableValues(callChildren.front(), lambdaTargets,
                                  variableTargets, *state.networkApiTargets,
                                  *state.extraApis);
            call.callbackTargets.insert(call.callbackTargets.end(),
                                        lambdaTargets.begin(),
                                        lambdaTargets.end());
        }
        for (std::size_t i = 1; i < callChildren.size(); ++i) {
            std::vector<std::string> functionTargets;
            std::vector<std::string> variableTargets;
            collectCallableValues(callChildren[i], functionTargets,
                                  variableTargets, *state.networkApiTargets,
                                  *state.extraApis);
            call.argumentTargets.push_back(std::move(functionTargets));
            call.argumentVariables.push_back(std::move(variableTargets));
            if (containsLambda(callChildren[i])) {
                const std::vector<std::string>& targets =
                    call.argumentTargets.back();
                call.callbackTargets.insert(call.callbackTargets.end(),
                                            targets.begin(), targets.end());
            }
        }
        call.loops = state.activeLoops;
        state.function->calls.push_back(std::move(call));
    }

    for (CXCursor child : children(cursor))
        collectCalls(child, state);
}

void analyzeTranslationUnit(
    CXTranslationUnit translationUnit,
    const std::vector<std::string>& extraApis,
    std::unordered_map<std::string, FunctionInfo>& functions,
    std::vector<std::string>& errors,
    std::unordered_map<std::string, std::unordered_set<std::string>>&
        functionPointerTargets,
    std::unordered_map<std::string, std::unordered_set<std::string>>&
        functionPointerAliases,
    std::unordered_map<std::string, std::string>& networkApiTargets) {
    const unsigned diagnosticCount =
        clang_getNumDiagnostics(translationUnit);
    for (unsigned i = 0; i < diagnosticCount; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(translationUnit, i);
        if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
            errors.push_back(toStd(clang_formatDiagnostic(
                diagnostic, clang_defaultDiagnosticDisplayOptions())));
        }
        clang_disposeDiagnostic(diagnostic);
    }
    if (!errors.empty())
        return;

    collectFunctions(clang_getTranslationUnitCursor(translationUnit),
                     functions);
    std::unordered_map<std::string, CXCursor> definitions;
    collectDefinitions(clang_getTranslationUnitCursor(translationUnit),
                       definitions);
    for (const auto& entry : definitions)
        collectFunctionPointerTargets(entry.second, functionPointerTargets,
                                      functionPointerAliases,
                                      networkApiTargets, extraApis);
    for (const auto& entry : definitions) {
        auto found = functions.find(entry.first);
        if (found == functions.end())
            continue;
        WalkState state{&found->second, &extraApis, &networkApiTargets, {}};
        collectCalls(entry.second, state, true);
    }
}

void collectDefinitions(CXCursor cursor,
                        std::unordered_map<std::string, CXCursor>& definitions) {
    if (isFunctionKind(clang_getCursorKind(cursor)) &&
        clang_isCursorDefinition(cursor) && !isSystemHeader(cursor))
        definitions.emplace(functionKey(cursor), cursor);
    for (CXCursor child : children(cursor))
        collectDefinitions(child, definitions);
}

void resolveFunctionPointerTargets(
    const std::unordered_map<std::string, FunctionInfo>& functions,
    std::unordered_map<std::string, std::unordered_set<std::string>>& targets,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        aliases) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& alias : aliases) {
            auto& targetSet = targets[alias.first];
            for (const std::string& source : alias.second) {
                const auto sourceTargets = targets.find(source);
                if (sourceTargets != targets.end()) {
                    for (const std::string& target : sourceTargets->second)
                        changed |= targetSet.insert(target).second;
                }
            }
        }
        for (const auto& function : functions) {
            for (const CallSite& call : function.second.calls) {
                for (std::size_t i = 0;
                     i < call.parameterKeys.size() &&
                     i < call.argumentTargets.size();
                     ++i) {
                    if (call.parameterKeys[i].empty())
                        continue;
                    for (const std::string& target : call.argumentTargets[i])
                        changed |= targets[call.parameterKeys[i]]
                                       .insert(target)
                                       .second;
                    if (i < call.argumentVariables.size()) {
                        for (const std::string& variable :
                             call.argumentVariables[i]) {
                            const auto argumentTargets = targets.find(variable);
                            if (argumentTargets == targets.end())
                                continue;
                            for (const std::string& target :
                                 argumentTargets->second)
                                changed |= targets[call.parameterKeys[i]]
                                               .insert(target)
                                               .second;
                        }
                    }
                }
            }
        }
    }
}

bool findNetworkPath(
    const std::string& key,
    const std::unordered_map<std::string, FunctionInfo>& functions,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        functionPointerTargets,
    const std::unordered_map<std::string, std::string>& networkApiTargets,
    std::unordered_set<std::string>& active, std::vector<std::string>& path,
    std::string& networkApi) {
    if (key.empty() || !active.insert(key).second)
        return false;
    const auto api = networkApiTargets.find(key);
    if (api != networkApiTargets.end()) {
        path = {api->second};
        networkApi = api->second;
        active.erase(key);
        return true;
    }
    const auto found = functions.find(key);
    if (found == functions.end()) {
        active.erase(key);
        return false;
    }

    for (const CallSite& call : found->second.calls) {
        if (!call.networkApi.empty()) {
            path = {found->second.name, call.networkApi};
            networkApi = call.networkApi;
            active.erase(key);
            return true;
        }
        std::vector<std::string> childPath;
        std::vector<std::string> targets;
        if (!call.calleeKey.empty())
            targets.push_back(call.calleeKey);
        const auto pointerTargets =
            functionPointerTargets.find(call.calleeVariableKey);
        if (pointerTargets != functionPointerTargets.end())
            targets.insert(targets.end(), pointerTargets->second.begin(),
                           pointerTargets->second.end());
        targets.insert(targets.end(), call.callbackTargets.begin(),
                       call.callbackTargets.end());
        bool foundNetwork = false;
        for (const std::string& target : targets) {
            if (findNetworkPath(target, functions, functionPointerTargets,
                                networkApiTargets, active, childPath,
                                networkApi)) {
                foundNetwork = true;
                break;
            }
        }
        if (foundNetwork) {
            path.clear();
            path.push_back(found->second.name);
            path.insert(path.end(), childPath.begin(), childPath.end());
            active.erase(key);
            return true;
        }
    }
    active.erase(key);
    return false;
}

std::vector<Finding> makeFindings(
    const std::unordered_map<std::string, FunctionInfo>& functions,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        functionPointerTargets,
    const std::unordered_map<std::string, std::string>& networkApiTargets) {
    std::vector<Finding> findings;
    std::set<std::string> seen;
    for (const auto& entry : functions) {
        const FunctionInfo& function = entry.second;
        for (const CallSite& call : function.calls) {
            if (call.loops.empty())
                continue;
            std::vector<std::string> path;
            std::string api;
            if (!call.networkApi.empty()) {
                path = {function.name, call.networkApi};
                api = call.networkApi;
            } else {
                std::unordered_set<std::string> active;
                std::vector<std::string> targets;
                if (!call.calleeKey.empty())
                    targets.push_back(call.calleeKey);
                const auto pointerTargets =
                    functionPointerTargets.find(call.calleeVariableKey);
                if (pointerTargets != functionPointerTargets.end())
                    targets.insert(targets.end(),
                                   pointerTargets->second.begin(),
                                   pointerTargets->second.end());
                targets.insert(targets.end(), call.callbackTargets.begin(),
                               call.callbackTargets.end());
                bool foundNetwork = false;
                for (const std::string& target : targets) {
                    if (findNetworkPath(target, functions,
                                        functionPointerTargets,
                                        networkApiTargets, active, path, api)) {
                        foundNetwork = true;
                        break;
                    }
                }
                if (!foundNetwork)
                    continue;
                path.insert(path.begin(), function.name);
            }
            for (const Location& loop : call.loops) {
                const std::string identity =
                    function.key + ":" + loop.file + ":" +
                    std::to_string(loop.line) + ":" +
                    std::to_string(loop.column) + ":" + call.location.file +
                    ":" + std::to_string(call.location.line) + ":" +
                    std::to_string(call.location.column) + ":" + api;
                if (!seen.insert(identity).second)
                    continue;
                findings.push_back({function.name, call.calleeName, api, path,
                                    loop, call.location});
            }
        }
    }
    std::sort(findings.begin(), findings.end(),
              [](const Finding& left, const Finding& right) {
                  if (left.loopLocation.file != right.loopLocation.file)
                      return left.loopLocation.file < right.loopLocation.file;
                  if (left.loopLocation.line != right.loopLocation.line)
                      return left.loopLocation.line < right.loopLocation.line;
                  return left.callLocation.line < right.callLocation.line;
              });
    return findings;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                result += escaped;
            } else {
                result += static_cast<char>(ch);
            }
        }
    }
    return result;
}

void writeLocation(std::ostream& out, const Location& location) {
    out << "{\"file\":\"" << jsonEscape(location.file)
        << "\",\"line\":" << location.line
        << ",\"column\":" << location.column << "}";
}

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--format text|json] [--output file] "
                 "[--api name] <source>... [-- clang args]\n",
                 program);
}

bool parseArgs(int argc, char** argv, Options& options, std::string& error) {
    bool clangArgs = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (clangArgs) {
            options.clangArgs.push_back(arg);
        } else if (arg == "--") {
            clangArgs = true;
        } else if (arg == "--format" || arg == "--output" ||
                   arg == "--api") {
            if (++i >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            const std::string value = argv[i];
            if (arg == "--format") {
                if (value != "text" && value != "json") {
                    error = "unknown format '" + value + "'";
                    return false;
                }
                options.format = value;
            } else if (arg == "--output") {
                options.output = value;
            } else {
                options.extraApis.push_back(value);
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg.rfind("--", 0) == 0) {
            error = "unknown option: " + arg;
            return false;
        } else {
            options.inputs.push_back(arg);
        }
    }
    if (options.inputs.empty()) {
        error = "no source file given";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parseArgs(argc, argv, options, error)) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        printUsage(argv[0]);
        return 1;
    }

    CXIndex index = clang_createIndex(0, 0);
    std::unordered_map<std::string, FunctionInfo> functions;
    std::vector<std::string> errors;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        functionPointerTargets;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        functionPointerAliases;
    std::unordered_map<std::string, std::string> networkApiTargets;
    for (const std::string& input : options.inputs) {
        std::vector<const char*> args;
        args.reserve(options.clangArgs.size());
        for (const std::string& arg : options.clangArgs)
            args.push_back(arg.c_str());
        CXTranslationUnit translationUnit = nullptr;
        const CXErrorCode result = clang_parseTranslationUnit2(
            index, input.c_str(), args.data(), static_cast<int>(args.size()),
            nullptr, 0, CXTranslationUnit_KeepGoing, &translationUnit);
        if (result != CXError_Success || !translationUnit) {
            errors.push_back("failed to parse source file: " + input);
            continue;
        }
        analyzeTranslationUnit(translationUnit, options.extraApis, functions,
                               errors, functionPointerTargets,
                               functionPointerAliases, networkApiTargets);
        clang_disposeTranslationUnit(translationUnit);
    }
    clang_disposeIndex(index);
    if (!errors.empty()) {
        for (const std::string& diagnostic : errors)
            std::fprintf(stderr, "[ERROR] %s\n", diagnostic.c_str());
        return 1;
    }

    resolveFunctionPointerTargets(functions, functionPointerTargets,
                                  functionPointerAliases);
    const std::vector<Finding> findings =
        makeFindings(functions, functionPointerTargets, networkApiTargets);
    std::ofstream file;
    std::ostream* output = &std::cout;
    if (!options.output.empty()) {
        file.open(options.output);
        if (!file) {
            std::fprintf(stderr, "[ERROR] cannot write output file: %s\n",
                         options.output.c_str());
            return 1;
        }
        output = &file;
    }

    if (options.format == "json") {
        *output << "{\"has_looped_network_calls\":"
                << (findings.empty() ? "false" : "true")
                << ",\"findings\":[";
        for (std::size_t i = 0; i < findings.size(); ++i) {
            if (i)
                *output << ",";
            const Finding& finding = findings[i];
            *output << "{\"function\":\"" << jsonEscape(finding.function)
                    << "\",\"call\":\"" << jsonEscape(finding.call)
                    << "\",\"network_api\":\""
                    << jsonEscape(finding.networkApi) << "\",\"loop\":";
            writeLocation(*output, finding.loopLocation);
            *output << ",\"call_site\":";
            writeLocation(*output, finding.callLocation);
            *output << ",\"call_chain\":[";
            for (std::size_t j = 0; j < finding.chain.size(); ++j) {
                if (j)
                    *output << ",";
                *output << "\"" << jsonEscape(finding.chain[j]) << "\"";
            }
            *output << "]}";
        }
        *output << "]}\n";
    } else if (findings.empty()) {
        *output << "No network service calls found in loop bodies.\n";
    } else {
        for (const Finding& finding : findings) {
            *output << finding.loopLocation.file << ":"
                    << finding.loopLocation.line << ": loop in "
                    << finding.function << " calls " << finding.call
                    << " at " << finding.callLocation.line
                    << ", reaching " << finding.networkApi << " via ";
            for (std::size_t i = 0; i < finding.chain.size(); ++i) {
                if (i)
                    *output << " -> ";
                *output << finding.chain[i];
            }
            *output << "\n";
        }
    }
    if (!*output) {
        std::fprintf(stderr, "[ERROR] failed while writing report\n");
        return 1;
    }
    return 0;
}
