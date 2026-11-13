
// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"




class  CypherLexer : public antlr4::Lexer {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, T__4 = 5, T__5 = 6, T__6 = 7, 
    T__7 = 8, T__8 = 9, T__9 = 10, T__10 = 11, T__11 = 12, T__12 = 13, T__13 = 14, 
    T__14 = 15, T__15 = 16, T__16 = 17, T__17 = 18, T__18 = 19, T__19 = 20, 
    T__20 = 21, T__21 = 22, T__22 = 23, T__23 = 24, T__24 = 25, T__25 = 26, 
    T__26 = 27, T__27 = 28, T__28 = 29, T__29 = 30, T__30 = 31, T__31 = 32, 
    T__32 = 33, T__33 = 34, T__34 = 35, T__35 = 36, T__36 = 37, T__37 = 38, 
    T__38 = 39, T__39 = 40, T__40 = 41, T__41 = 42, T__42 = 43, T__43 = 44, 
    T__44 = 45, T__45 = 46, ACYCLIC = 47, ANY = 48, ADD = 49, ALL = 50, 
    ALTER = 51, AND = 52, AS = 53, ASC = 54, ASCENDING = 55, ATTACH = 56, 
    BEGIN = 57, BY = 58, CALL = 59, CASE = 60, CAST = 61, CHECKPOINT = 62, 
    COLUMN = 63, COMMENT = 64, COMPACT = 65, COMMIT = 66, COMMIT_SKIP_CHECKPOINT = 67, 
    CONTAINS = 68, COPY = 69, COUNT = 70, CREATE = 71, CYCLE = 72, DATABASE = 73, 
    DBTYPE = 74, DEFAULT = 75, DELETE = 76, DESC = 77, DESCENDING = 78, 
    DETACH = 79, DISTINCT = 80, DROP = 81, ELSE = 82, END = 83, ENDS = 84, 
    EXISTS = 85, EXPLAIN = 86, EXPORT = 87, EXTENSION = 88, FROM = 89, GLOB = 90, 
    GRAPH = 91, GROUP = 92, HEADERS = 93, HINT = 94, IMPORT = 95, IF = 96, 
    IN = 97, INCREMENT = 98, INDEX = 99, INSTALL = 100, IS = 101, JOIN = 102, 
    KEY = 103, LIMIT = 104, LOAD = 105, LOGICAL = 106, MACRO = 107, MATCH = 108, 
    MAXVALUE = 109, MERGE = 110, MINVALUE = 111, MULTI_JOIN = 112, NAMESPACE = 113, 
    NO = 114, NODE = 115, NOT = 116, NONE = 117, NULL_ = 118, ON = 119, 
    ONLY = 120, OPTIONAL = 121, OR = 122, ORDER = 123, PRIMARY = 124, PROFILE = 125, 
    PROJECT = 126, READ = 127, REL = 128, RENAME = 129, RETURN = 130, ROLLBACK = 131, 
    ROLLBACK_SKIP_CHECKPOINT = 132, SEQUENCE = 133, SET = 134, SHORTEST = 135, 
    START = 136, STARTS = 137, TABLE = 138, TEMP = 139, THEN = 140, TO = 141, 
    TRAIL = 142, TRANSACTION = 143, TYPE = 144, UNINSTALL = 145, UNION = 146, 
    USING = 147, UNWIND = 148, USE = 149, WHEN = 150, WHERE = 151, WITH = 152, 
    WRITE = 153, WSHORTEST = 154, XOR = 155, SINGLE = 156, YIELD = 157, 
    DECIMAL = 158, VARCHAR = 159, STAR = 160, L_SKIP = 161, INVALID_NOT_EQUAL = 162, 
    MINUS = 163, FACTORIAL = 164, COLON = 165, BTRUE = 166, BFALSE = 167, 
    StringLiteral = 168, EscapedChar = 169, DecimalInteger = 170, HexLetter = 171, 
    HexDigit = 172, Digit = 173, NonZeroDigit = 174, NonZeroOctDigit = 175, 
    ZeroDigit = 176, ExponentDecimalReal = 177, RegularDecimalReal = 178, 
    UnescapedSymbolicName = 179, IdentifierStart = 180, IdentifierPart = 181, 
    EscapedSymbolicName = 182, SP = 183, WHITESPACE = 184, CypherComment = 185, 
    Unknown = 186
  };

  explicit CypherLexer(antlr4::CharStream *input);

  ~CypherLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

