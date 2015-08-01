/*
 * llvmtypecheck.cpp
 *
 *  Created on: Jul 23, 2015
 *      Author: mramalho
 */

#include "llvm_convert.h"

#include <std_code.h>
#include <std_expr.h>
#include <expr_util.h>

#include <ansi-c/c_types.h>
#include <ansi-c/convert_integer_literal.h>

#include <boost/filesystem.hpp>

std::string repeat( const std::string &word, int times ) {
   std::string result ;
   result.reserve(times*word.length()); // avoid repeated reallocation
   for (int a = 0 ; a < times ; a++)
      result += word ;
   return result ;
}

llvm_convertert::llvm_convertert(contextt &_context)
  : context(_context),
    ns(context),
    current_location(locationt()),
    current_path(""),
    current_function_name(""),
    current_scope(0),
    sm(nullptr)
{
}

llvm_convertert::~llvm_convertert()
{
}

bool llvm_convertert::convert()
{
  if(convert_top_level_decl())
    return true;

  return false;
}

bool llvm_convertert::convert_top_level_decl()
{
  // Iterate through each translation unit and their global symbols, creating
  // symbols as we go.
  for (auto &translation_unit : ASTs)
  {
    for (clang::ASTUnit::top_level_iterator
      it = translation_unit->top_level_begin();
      it != translation_unit->top_level_end();
      it++)
    {
      set_source_manager((*it)->getASTContext().getSourceManager());
      update_current_location((*it)->getLocation());

      exprt dummy_decl;
      convert_decl(**it, dummy_decl);
    }
  }

  return false;
}

void llvm_convertert::convert_decl(
  const clang::Decl& decl,
  exprt &new_expr)
{
  switch (decl.getKind()) {
    case clang::Decl::Typedef:
    {
      const clang::TypedefDecl &tdd =
        static_cast<const clang::TypedefDecl&>(decl);
      convert_typedef(tdd, new_expr);
      break;
    }

    case clang::Decl::Var:
    {
      const clang::VarDecl &vd =
        static_cast<const clang::VarDecl&>(decl);
      convert_var(vd, new_expr);
      break;
    }

    case clang::Decl::Function:
    {
      const clang::FunctionDecl &fd =
        static_cast<const clang::FunctionDecl&>(decl);
      convert_function(fd);
      break;
    }

    // Apparently if you insert a semicolon at the end of a
    // function declaration, this AST is created, so just
    // ignore it
    case clang::Decl::Empty:
      break;

    case clang::Decl::Record:
    default:
      std::cerr << "Unrecognized / unimplemented decl type ";
      std::cerr << decl.getDeclKindName() << std::endl;
      abort();
  }

  new_expr.location() = current_location;
}

void llvm_convertert::convert_typedef(
  const clang::TypedefDecl &tdd,
  exprt &new_expr)
{
  symbolt symbol;
  get_default_symbol(symbol);

  clang::QualType q_type = tdd.getUnderlyingType();

  // Get type
  typet t;
  get_type(q_type, t);

  symbol.type = t;
  symbol.base_name = tdd.getName().str();
  symbol.pretty_name =
    symbol.module.as_string() + "::" + symbol.base_name.as_string();
  symbol.name =
    "c::" + symbol.module.as_string() + "::" + symbol.base_name.as_string();
  symbol.is_type = true;

  if (context.move(symbol)) {
    std::cerr << "Couldn't add symbol " << symbol.name
              << " to symbol table" << std::endl;
    abort();
  }

  if(current_function_name!= "")
    new_expr = code_skipt();
}

void llvm_convertert::convert_var(
  const clang::VarDecl &vd,
  exprt &new_expr)
{
  symbolt symbol;
  get_default_symbol(symbol);

  clang::QualType q_type = vd.getType();

  // Get type
  typet t;
  get_type(q_type, t);

  symbol.type = t;
  symbol.base_name = vd.getName().str();

  // This is not local, so has static lifetime
  if (!vd.hasLocalStorage())
  {
    symbol.static_lifetime = true;
    symbol.value = gen_zero(t);

    // Add location to value since it is only added on get_expr
    symbol.value.location() = current_location;
  }

  irep_idt identifier =
    get_var_name(symbol.base_name.as_string(), vd.hasLocalStorage());

  symbol.pretty_name = identifier;
  symbol.name = "c::" + symbol.pretty_name.as_string();

  if (vd.hasExternalStorage())
    symbol.is_extern = true;
  symbol.lvalue = true;

  // We have to add the symbol before converting the initial assignment
  // because we might have something like 'int x = x + 1;' which is
  // completely wrong but allowed by the language
  if (context.move(symbol)) {
    std::cerr << "Couldn't add symbol " << symbol.name
              << " to symbol table" << std::endl;
    abort();
  }

  // Now get the symbol back to continue the conversion
  // The problem is that lookup returns a const symbolt,
  // so const_cast it to symbolt, so we can add the value
  // Maybe this could be avoided if we had an set_value method?
  symbolt &added_symbol =
    const_cast<symbolt&>(ns.lookup("c::" + identifier.as_string()));

  code_declt decl;
  decl.operands().push_back(symbol_expr(added_symbol));

  if(vd.hasInit())
  {
    const clang::Expr *value = vd.getInit();
    exprt val;
    get_expr(*value, val);

    added_symbol.value = val;
    decl.operands().push_back(val);
  }

  new_expr = decl;
}

void llvm_convertert::convert_function(const clang::FunctionDecl &fd)
{
  std::string old_function_name = current_function_name;

  symbolt symbol;
  get_default_symbol(symbol);

  symbol.base_name = fd.getName().str();
  symbol.name = "c::" + symbol.base_name.as_string();
  symbol.pretty_name = symbol.base_name.as_string();
  symbol.lvalue = true;

  current_function_name = fd.getName().str();

  // We need: a type, a name, and an optional body
  clang::Stmt *body = NULL;
  if (fd.isThisDeclarationADefinition() && fd.hasBody())
    body = fd.getBody();

  // Build function's type
  code_typet type;

  // Return type
  const clang::QualType ret_type = fd.getReturnType();
  typet return_type;
  get_type(ret_type, return_type);
  type.return_type() = return_type;

  // We convert the parameters first so their symbol are added to context
  // before converting the body, as they may appear on the function body
  if(body)
  {
    for (const auto &pdecl : fd.params()) {
      code_typet::argumentt param =
        convert_function_params(symbol.base_name.as_string(), pdecl);
      type.arguments().push_back(param);
    }

    get_expr(*body, symbol.value);
  }

  // Set the end location for functions, we get all the information
  // from the current location (file, line and function name) then
  // we change the line number
  locationt end_location;
  end_location = current_location;
  end_location.set_line(sm->getSpellingLineNumber(fd.getLocEnd()));
  symbol.value.end_location(end_location);

  // And the location
  type.location() = symbol.location;
  symbol.type = type;

  if (context.move(symbol)) {
    std::cerr << "Couldn't add symbol " << symbol.name
              << " to symbol table" << std::endl;
    abort();
  }

  current_function_name = old_function_name;
}

code_typet::argumentt llvm_convertert::convert_function_params(
  std::string function_name,
  clang::ParmVarDecl *pdecl)
{
  symbolt param_symbol;
  get_default_symbol(param_symbol);

  const clang::QualType q_type = pdecl->getOriginalType();
  typet param_type;
  get_type(q_type, param_type);

  param_symbol.type = param_type;

  std::string name = pdecl->getName().str();
  param_symbol.base_name = name;
  param_symbol.pretty_name =
    get_param_name(param_symbol.base_name.as_string());
  param_symbol.name = "c::" + param_symbol.pretty_name.as_string();

  param_symbol.lvalue = true;
  param_symbol.file_local = true;
  param_symbol.is_actual = true;

  code_typet::argumentt arg;
  arg.type() = param_type;
  arg.base_name(name);
  arg.cmt_identifier(param_symbol.name.as_string());
  arg.location() = param_symbol.location;

  if (context.move(param_symbol)) {
    std::cerr << "Couldn't add symbol " << param_symbol.name
        << " to symbol table" << std::endl;
    abort();
  }

  return arg;
}

void llvm_convertert::get_type(const clang::QualType &q_type, typet &new_type)
{
  const clang::Type &the_type = *q_type.getTypePtrOrNull();
  clang::Type::TypeClass tc = the_type.getTypeClass();
  switch (tc) {
    case clang::Type::Builtin:
    {
      const clang::BuiltinType &bt = static_cast<const clang::BuiltinType&>(the_type);
      switch (bt.getKind()) {
        case clang::BuiltinType::Void:
          new_type = empty_typet();
          break;

        case clang::BuiltinType::Bool:
          new_type = bool_type();
          break;

        case clang::BuiltinType::UChar:
          new_type = unsignedbv_typet(config.ansi_c.char_width);
          break;

        case clang::BuiltinType::Char16:
          new_type = unsignedbv_typet(16);
          break;

        case clang::BuiltinType::Char32:
          new_type = unsignedbv_typet(32);
          break;

        case clang::BuiltinType::UShort:
          new_type = unsignedbv_typet(config.ansi_c.short_int_width);
          break;

        case clang::BuiltinType::UInt:
          new_type = uint_type();
          break;

        case clang::BuiltinType::ULong:
          new_type = long_uint_type();
          break;

        case clang::BuiltinType::ULongLong:
          new_type = long_long_uint_type();
          break;

        case clang::BuiltinType::UInt128:
          // Various simplification / big-int related things use uint64_t's...
          std::cerr << "No support for uint128's in ESBMC right now, sorry" << std::endl;
          abort();

        case clang::BuiltinType::SChar:
          new_type = signedbv_typet(config.ansi_c.char_width);
          break;

        case clang::BuiltinType::Short:
          new_type = signedbv_typet(config.ansi_c.short_int_width);
          break;

        case clang::BuiltinType::Int:
          new_type = int_type();
          break;

        case clang::BuiltinType::Long:
          new_type = long_int_type();
          break;

        case clang::BuiltinType::LongLong:
          new_type = long_long_int_type();
          break;

        case clang::BuiltinType::Int128:
          // Various simplification / big-int related things use uint64_t's...
          std::cerr << "No support for uint128's in ESBMC right now, sorry" << std::endl;
          abort();

        case clang::BuiltinType::Float:
          new_type = float_type();
          break;

        case clang::BuiltinType::Double:
          new_type = double_type();
          break;

        case clang::BuiltinType::LongDouble:
          new_type = long_double_type();
          break;

        default:
          std::cerr << "Unrecognized clang builtin type "
                    << bt.getName(clang::PrintingPolicy(clang::LangOptions())).str()
                    << std::endl;
          abort();
      }
      break;
    }

    case clang::Type::Typedef:
    {
      const clang::TypedefType &pt =
        static_cast<const clang::TypedefType &>(the_type);
      clang::QualType q_typedef_type = pt.getDecl()->getUnderlyingType();
      get_type(q_typedef_type, new_type);
      break;
    }

    case clang::Type::Pointer:
    {
      const clang::PointerType &pt =
        static_cast<const clang::PointerType &>(the_type);
      const clang::QualType &pointee = pt.getPointeeType();
      get_type(pointee, new_type);
      break;
    }

    // Those two here appears when we make a function call, e.g:
    // FunctionNoProto: int x = fun()
    // FunctionProto: int x = fun(a, b)
    case clang::Type::FunctionNoProto:
    case clang::Type::FunctionProto:
      break;

    default:
      std::cerr << "No clang <=> ESBMC migration for type "
                << the_type.getTypeClassName() << std::endl;
      the_type.dump();
      abort();
  }

  if(q_type.isConstQualified())
    new_type.cmt_constant(true);
}

void llvm_convertert::get_expr(
  const clang::Stmt& stmt,
  exprt& new_expr)
{
  update_current_location(stmt.getLocStart());

  switch(stmt.getStmtClass()) {
    case clang::Stmt::IntegerLiteralClass:
    {
      const clang::IntegerLiteral &integer =
        static_cast<const clang::IntegerLiteral&>(stmt);
      llvm::APInt val = integer.getValue();
      assert(val.getBitWidth() <= 64 && "Too large an integer found, sorry");

      typet the_type;
      get_type(integer.getType(), the_type);
      assert(the_type.is_unsignedbv() || the_type.is_signedbv());

      exprt bval;
      if (the_type.is_unsignedbv()) {
        uint64_t the_val = val.getZExtValue();
        convert_integer_literal(integer2string(the_val) + "u", bval, 10);
      } else {
        int64_t the_val = val.getSExtValue();
        convert_integer_literal(integer2string(the_val), bval, 10);
      }

      new_expr.swap(bval);
      break;
    }

    case clang::Stmt::ImplicitCastExprClass:
    case clang::Stmt::CStyleCastExprClass:
    {
      const clang::CastExpr &cast =
        static_cast<const clang::CastExpr &>(stmt);
      get_cast_expr(cast, new_expr);
      break;
    }

    case clang::Stmt::CompoundStmtClass:
    {
      const clang::CompoundStmt &compound_stmt =
        static_cast<const clang::CompoundStmt &>(stmt);

      // Increase current scope number, it will be used for variables' name
      // This will be increased every time a block is parsed
      assert(current_scope >= 0);
      ++current_scope;

      code_blockt block;
      for (const auto &stmt : compound_stmt.body()) {
        exprt statement;
        get_expr(*stmt, statement);
        block.operands().push_back(statement);
      }
      new_expr = block;

      --current_scope;
      assert(current_scope >= 0);
      break;
    }

    case clang::Stmt::DeclStmtClass:
    {
      const clang::DeclStmt &decl =
        static_cast<const clang::DeclStmt&>(stmt);

      const auto &declgroup = decl.getDeclGroup();

      codet decls("decl-block");
      for (clang::DeclGroupRef::const_iterator
        it = declgroup.begin();
        it != declgroup.end();
        it++)
      {
        exprt single_decl;
        convert_decl(**it, single_decl);
        decls.operands().push_back(single_decl);
      }

      new_expr = decls;
      break;
    }

    case clang::Stmt::BinaryOperatorClass:
    {
      const clang::BinaryOperator &binop =
        static_cast<const clang::BinaryOperator&>(stmt);
      get_binary_operator_expr(binop, new_expr);
      break;
    }

    case clang::Stmt::DeclRefExprClass:
    {
      const clang::DeclRefExpr &decl =
        static_cast<const clang::DeclRefExpr&>(stmt);

      const clang::Decl &dcl =
        static_cast<const clang::Decl&>(*decl.getDecl());

      get_decl_expr(dcl, new_expr);
      break;
    }

    case clang::Stmt::ReturnStmtClass:
    {
      const clang::ReturnStmt &ret =
        static_cast<const clang::ReturnStmt&>(stmt);

      const clang::Expr &retval = *ret.getRetValue();
      exprt val;
      get_expr(retval, val);

      code_returnt ret_expr;
      ret_expr.return_value() = val;

      new_expr = ret_expr;
      break;
    }

    case clang::Stmt::CallExprClass:
    {
      const clang::CallExpr &function_call =
        static_cast<const clang::CallExpr &>(stmt);

      const clang::Expr *callee = function_call.getCallee();
      exprt callee_expr;
      get_expr(*callee, callee_expr);

      side_effect_expr_function_callt call;
      call.function() = callee_expr;

      for (const clang::Expr *arg : function_call.arguments()) {
        exprt single_arg;
        get_expr(*arg, single_arg);
        call.arguments().push_back(single_arg);
      }

      new_expr = call;
      break;
    }

    default:
      std::cerr << "Conversion of unsupported clang expr: \"";
      std::cerr << stmt.getStmtClassName() << "\" to expression" << std::endl;
      stmt.dump();
      abort();
  }

  new_expr.location() = current_location;
}

void llvm_convertert::get_decl_expr(
  const clang::Decl& decl,
  exprt& new_expr)
{
  irep_idt identifier;

  switch(decl.getKind())
  {
    case clang::Decl::Var:
    {
      const clang::VarDecl &vd =
        static_cast<const clang::VarDecl&>(decl);

      identifier =
        "c::" + get_var_name(vd.getName().str(), vd.hasLocalStorage());
      break;
    }

    case clang::Decl::ParmVar:
    {
      const clang::VarDecl &vd =
        static_cast<const clang::VarDecl&>(decl);
      identifier = "c::" + get_param_name(vd.getName().str());
      break;
    }

    case clang::Decl::Function:
    {
      const clang::FunctionDecl &fd =
        static_cast<const clang::FunctionDecl&>(decl);
      identifier = "c::" + fd.getName().str();
      break;
    }

    default:
      std::cerr << "Conversion of unsupported clang decl operator: \"";
      std::cerr << decl.getDeclKindName() << "\" to expression" << std::endl;
      decl.dump();
      abort();
  }

  const symbolt &symbol = ns.lookup(identifier);
  new_expr = symbol_expr(symbol);
}

void llvm_convertert::get_cast_expr(
  const clang::CastExpr& cast,
  exprt& new_expr)
{
  exprt expr;
  get_expr(*cast.getSubExpr(), expr);

  typet type;
  get_type(cast.getType(), type);

  switch(cast.getCastKind())
  {
    case clang::CK_FunctionToPointerDecay:
      break;

    case clang::CK_IntegralCast:
    case clang::CK_IntegralToBoolean:
    case clang::CK_IntegralToFloating:

    case clang::CK_FloatingToIntegral:
    case clang::CK_FloatingToBoolean:
    case clang::CK_FloatingCast:

    case clang::CK_LValueToRValue:
      gen_typecast(expr, type);
      break;

    default:
      std::cerr << "Conversion of unsupported clang cast operator: \"";
      std::cerr << cast.getCastKindName() << "\" to expression" << std::endl;
      cast.dumpColor();
      abort();
  }

  new_expr = expr;
}

void llvm_convertert::get_binary_operator_expr(
  const clang::BinaryOperator& binop,
  exprt& new_expr)
{
  exprt lhs;
  get_expr(*binop.getLHS(), lhs);

  exprt rhs;
  get_expr(*binop.getRHS(), rhs);

  switch(binop.getOpcode())
  {
    case clang::BO_Assign:
      new_expr = codet("assign");
      gen_typecast(rhs, lhs.type());
      break;

    case clang::BO_Add:
      new_expr = exprt("+");
      break;

    case clang::BO_Sub:
      new_expr = exprt("-");
      break;

    case clang::BO_Mul:
      new_expr = exprt("*");
      break;

    case clang::BO_Div:
      new_expr = exprt("/");
      break;

    case clang::BO_Shl:
      new_expr = exprt("shl");
      break;

    case clang::BO_Shr:
      new_expr = exprt("ashr");
      break;

    case clang::BO_Rem:
      new_expr = exprt("mod");
      break;

    case clang::BO_And:
      new_expr = exprt("bitand");
      break;

    case clang::BO_Xor:
      new_expr = exprt("bitxor");
      break;

    case clang::BO_Or:
      new_expr = exprt("bitor");
      break;

    case clang::BO_LT:
      new_expr = exprt("<");
      new_expr.type() = bool_type();
      break;

    case clang::BO_GT:
      new_expr = exprt(">");
      new_expr.type() = bool_type();
      break;

    case clang::BO_LE:
      new_expr = exprt("<=");
      new_expr.type() = bool_type();
      break;

    case clang::BO_GE:
      new_expr = exprt(">=");
      new_expr.type() = bool_type();
      break;

    case clang::BO_EQ:
      new_expr = exprt("=");
      new_expr.type() = bool_type();
      break;

    case clang::BO_NE:
      new_expr = exprt("notequal");
      new_expr.type() = bool_type();
      break;

    case clang::BO_LAnd:
      new_expr = exprt("and");
      new_expr.type() = bool_type();

      gen_typecast(lhs, bool_type());
      gen_typecast(rhs, bool_type());
      break;

    case clang::BO_LOr:
      new_expr = exprt("or");
      new_expr.type() = bool_type();

      gen_typecast(lhs, bool_type());
      gen_typecast(rhs, bool_type());
      break;

    default:
      std::cerr << "Conversion of unsupported clang binary operator: \"";
      std::cerr << binop.getOpcodeStr().str() << "\" to expression" << std::endl;
      binop.dumpColor();
      abort();
  }

  new_expr.copy_to_operands(lhs, rhs);
}

void llvm_convertert::gen_typecast(
  exprt &expr,
  typet type)
{
  if(expr.type() != type)
  {
    exprt new_expr;

    // I don't think that this code is really needed,
    // but it removes typecasts, which is good
    // It should simplify constants to either true or
    // false when casting them to bool
    if(type.is_bool() and expr.is_constant())
    {
      mp_integer value=string2integer(expr.cformat().as_string());
      if(value != 0)
        new_expr = true_exprt();
      else
       new_expr = false_exprt();
    }
    else
      new_expr = typecast_exprt(expr, type);

    new_expr.location() = expr.location();
    expr.swap(new_expr);
  }
}

void llvm_convertert::get_default_symbol(symbolt& symbol)
{
  symbol.mode = "C";
  symbol.module = get_modulename_from_path();
  symbol.location = current_location;
}

std::string llvm_convertert::get_var_name(
  std::string name,
  bool is_local)
{
  if(!is_local)
    return name;

  std::string pretty_name = get_modulename_from_path() + "::";
  if(current_function_name!= "")
    pretty_name += current_function_name + "::";
  if(current_scope > 0)
    pretty_name += repeat("1::", current_scope);
  pretty_name += name;

  return pretty_name;
}

std::string llvm_convertert::get_param_name(std::string name)
{
  std::string pretty_name = get_modulename_from_path() + "::";
  if(current_function_name!= "")
    pretty_name += current_function_name + "::";
  pretty_name += name;

  return pretty_name;
}

void llvm_convertert::set_source_manager(
  clang::SourceManager& source_manager)
{
  sm = &source_manager;
}

void llvm_convertert::update_current_location(
  clang::SourceLocation source_location)
{
  current_path =sm->getFilename(source_location).str();

  current_location.set_file(get_filename_from_path());
  current_location.set_line(sm->getSpellingLineNumber(source_location));
  current_location.set_function(current_function_name);
}

std::string llvm_convertert::get_modulename_from_path()
{
  return  boost::filesystem::path(current_path).stem().string();
}

std::string llvm_convertert::get_filename_from_path()
{
  return  boost::filesystem::path(current_path).filename().string();
}
