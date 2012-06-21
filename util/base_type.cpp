/*******************************************************************\

Module: Base Type Computation

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#include <assert.h>

#include "std_types.h"
#include "base_type.h"
#include "union_find.h"

/*******************************************************************\

Function: base_type

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void base_type(type2tc &type, const namespacet &ns)
{
  if (is_symbol_type(type))
  {
    const symbolt *symbol;

    if (!ns.lookup(to_symbol_type(type).symbol_name, symbol) &&
        symbol->is_type && !symbol->type.is_nil())
    {
      migrate_type(symbol->type, type);
      base_type(type, ns); // recursive call
      return;
    }
  }
  else if (is_array_type(type))
  {
    base_type(to_array_type(type).subtype, ns);
  }
  else if (is_struct_type(type) | is_union_type(type))
  {
    struct_union_data &data = static_cast<struct_union_data&>(*type.get());

    Forall_types(it, data.members) {
      type2tc &subtype = *it;
      base_type(subtype, ns);
    }
  }
}

void base_type(typet &type, const namespacet &ns)
{
  if(type.id()=="symbol")
  {
    const symbolt *symbol;

    if(!ns.lookup(type.identifier(), symbol) &&
       symbol->is_type &&
       !symbol->type.is_nil())
    {
      type=symbol->type;
      base_type(type, ns); // recursive call
      return;
    }
  }
  else if(type.id()=="subtype")
  {
    typet tmp;
    tmp.swap(type.subtype());
    type.swap(tmp);
  }
  else if(type.is_array())
  {
    base_type(type.subtype(), ns);
  }
  else if(type.id()=="struct" ||
          type.id()=="class" ||
          type.id()=="union")
  {
    // New subt for manipulating components
    irept::subt components=type.components().get_sub();

    Forall_irep(it, components)
    {
      typet &subtype=it->type();
      base_type(subtype, ns);
    }

    // Set back into type
    irept tmp = type.components();
    tmp.get_sub() = components;
    type.components(tmp);
  }
}

/*******************************************************************\

Function: base_type

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void base_type(expr2tc &expr, const namespacet &ns)
{
  base_type(expr.get()->type, ns);

  Forall_operands2(it, tmpops, expr)
    base_type(**it, ns);
}

void base_type(exprt &expr, const namespacet &ns)
{
  base_type(expr.type(), ns);

  Forall_operands(it, expr)
    base_type(*it, ns);
}

/*******************************************************************\

Function: base_type_eqt::base_type_eq_rec

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

bool base_type_eqt::base_type_eq_rec(
  const type2tc &type1,
  const type2tc &type2)
{
  if (type1==type2)
    return true;
    
  // loop avoidance
  if (is_symbol_type(type1) && is_symbol_type(type2))
  {
    // already in same set?
    if (identifiers.make_union(to_symbol_type(type1).symbol_name,
                               to_symbol_type(type2).symbol_name))
      return true;
  }

  if (is_symbol_type(type1))
  {
    const symbolt &symbol = ns.lookup(to_symbol_type(type1).symbol_name);

    if(!symbol.is_type)
      throw "symbol "+id2string(symbol.name)+" is not a type";
    
    type2tc tmp;
    migrate_type(symbol.type, tmp);
    return base_type_eq_rec(tmp, type2);
  }

  if (is_symbol_type(type2))
  {
    const symbolt &symbol=ns.lookup(to_symbol_type(type2).symbol_name);

    if(!symbol.is_type)
      throw "symbol "+id2string(symbol.name)+" is not a type";

    type2tc tmp;
    migrate_type(symbol.type, tmp);
    return base_type_eq_rec(type1, tmp);
  }
  
  if (type1->type_id != type2->type_id)
    return false;

  if (is_struct_type(type1) || is_union_type(type1)) {
    const struct_union_data &data1 =
      static_cast<const struct_union_data &>(*type1.get());
    const struct_union_data &data2 =
      static_cast<const struct_union_data &>(*type2.get());

    if (data1.members.size() != data2.members.size())
      return false;

    for (unsigned i=0; i < data1.members.size(); i++)
    {
      const type2tc &subtype1 = data1.members[i];
      const type2tc &subtype2 = data2.members[i];
      if (!base_type_eq_rec(subtype1, subtype2)) return false;
      if (data1.member_names[i] != data2.member_names[i]) return false;
    }
    
    return true;
  }
  else if (is_code_type(type1))
  {
    const code_type2t &code1 = to_code_type(type1);
    const code_type2t &code2 = to_code_type(type2);
    
    if (code1.arguments.size() != code2.arguments.size())
      return false;
      
    for (unsigned i=0; i < code1.arguments.size(); i++)
    {
      const type2tc &subtype1 = code1.arguments[i];
      const type2tc &subtype2 = code2.arguments[i];
      if (!base_type_eq_rec(subtype1, subtype2)) return false;
    }
    
    const type2tc &return_type1 = code1.ret_type;
    const type2tc &return_type2 = code2.ret_type;
    
    if (!base_type_eq_rec(return_type1, return_type2))
      return false;
    
    return true;
  }
  else if (is_pointer_type(type1))
  {
    return base_type_eq_rec(to_pointer_type(type1).subtype,
                            to_pointer_type(type2).subtype);
  }
  else if (is_array_type(type1))
  {
    if (!base_type_eq_rec(to_array_type(type1).subtype,
                          to_array_type(type2).subtype))
      return false;
      
    // TODO: check size
      
    return true;
  }

  type2tc tmp1(type1), tmp2(type2);

  base_type(tmp1, ns);
  base_type(tmp2, ns);

  return tmp1 == tmp2;  
}

bool base_type_eqt::base_type_eq_rec(
  const typet &type1,
  const typet &type2)
{
  if(type1==type2)
    return true;
    
  #if 0
  std::cout << "T1: " << type1.pretty() << std::endl;
  std::cout << "T2: " << type2.pretty() << std::endl;
  #endif
  
  // loop avoidance
  if(type1.id()=="symbol" &&
     type2.id()=="symbol")
  {
    // already in same set?
    if(identifiers.make_union(
         type1.identifier(),
         type2.identifier()))
      return true;
  }

  if(type1.id()=="symbol")
  {
    const symbolt &symbol=ns.lookup(type1.identifier());

    if(!symbol.is_type)
      throw "symbol "+id2string(symbol.name)+" is not a type";
    
    return base_type_eq_rec(symbol.type, type2);
  }

  if(type2.id()=="symbol")
  {
    const symbolt &symbol=ns.lookup(type2.identifier());

    if(!symbol.is_type)
      throw "symbol "+id2string(symbol.name)+" is not a type";

    return base_type_eq_rec(type1, symbol.type);
  }
  
  if(type1.id()!=type2.id())
    return false;

  if(type1.id()=="struct" ||
     type1.id()=="class" ||
     type1.id()=="union")
  {
    const struct_union_typet::componentst &components1=
      to_struct_union_type(type1).components();

    const struct_union_typet::componentst &components2=
      to_struct_union_type(type2).components();
      
    if(components1.size()!=components2.size())
      return false;

    for(unsigned i=0; i<components1.size(); i++)
    {
      const typet &subtype1=components1[i].type();
      const typet &subtype2=components2[i].type();
      if(!base_type_eq_rec(subtype1, subtype2)) return false;
      if(components1[i].get_name()!=components2[i].get_name()) return false;
    }
    
    return true;
  }
  else if(type1.id()=="incomplete_struct")
  {
    return true;
  }
  else if(type1.is_code())
  {
    const irept::subt &arguments1=type1.arguments().get_sub();

    const irept::subt &arguments2=type2.arguments().get_sub();
    
    if(arguments1.size()!=arguments2.size())
      return false;
      
    for(unsigned i=0; i<arguments1.size(); i++)
    {
      const typet &subtype1=arguments1[i].type();
      const typet &subtype2=arguments2[i].type();
      if(!base_type_eq_rec(subtype1, subtype2)) return false;
    }
    
    const typet &return_type1=(typet &)type1.return_type();
    const typet &return_type2=(typet &)type2.return_type();
    
    if(!base_type_eq_rec(return_type1, return_type2))
      return false;
    
    return true;
  }
  else if(type1.id()=="pointer")
  {
    return base_type_eq_rec(type1.subtype(), type2.subtype());
  }
  else if(type1.is_array())
  {
    if(!base_type_eq_rec(type1.subtype(), type2.subtype()))
      return false;
      
    // TODO: check size
      
    return true;
  }
  else if(type1.id()=="incomplete_array")
  {
    return base_type_eq_rec(type1.subtype(), type2.subtype());
  }

  typet tmp1(type1), tmp2(type2);

  base_type(tmp1, ns);
  base_type(tmp2, ns);

  return tmp1==tmp2;  
}

/*******************************************************************\

Function: base_type_eqt::base_type_eq_rec

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

bool base_type_eqt::base_type_eq_rec(
  const expr2tc &expr1,
  const expr2tc &expr2)
{
  if (expr1->expr_id != expr2->expr_id)
    return false;
    
  if (!base_type_eq(expr1->type, expr2->type))
    return false;

  expr2t::expr_operands ops1, ops2;
  expr1->list_operands(ops1);
  expr2->list_operands(ops2);

  if (ops1.size() != ops2.size())
    return false;
    
  expr2t::expr_operands::const_iterator it1 = ops1.begin();
  expr2t::expr_operands::const_iterator it2 = ops2.begin();
  for (; it1 != ops1.end(); it1++, it2++)
    if (!base_type_eq(**it1, **it2))
      return false;
  
  return true;
}

bool base_type_eqt::base_type_eq_rec(
  const exprt &expr1,
  const exprt &expr2)
{
  if(expr1.id()!=expr2.id())
    return false;
    
  if(!base_type_eq(expr1.type(), expr2.type()))
    return false;

  if(expr1.operands().size()!=expr2.operands().size())
    return false;
    
  for(unsigned i=0; i<expr1.operands().size(); i++)
    if(!base_type_eq(expr1.operands()[i], expr2.operands()[i]))
      return false;
  
  return true;
}

/*******************************************************************\

Function: base_type_eq

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

bool base_type_eq(
  const type2tc &type1,
  const type2tc &type2,
  const namespacet &ns)
{
  base_type_eqt base_type_eq(ns);
  return base_type_eq.base_type_eq(type1, type2);
}

bool base_type_eq(
  const typet &type1,
  const typet &type2,
  const namespacet &ns)
{
  base_type_eqt base_type_eq(ns);
  return base_type_eq.base_type_eq(type1, type2);
}

/*******************************************************************\

Function: base_type_eq

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

bool base_type_eq(
  const expr2tc &expr1,
  const expr2tc &expr2,
  const namespacet &ns)
{
  base_type_eqt base_type_eq(ns);
  return base_type_eq.base_type_eq(expr1, expr2);
}

bool base_type_eq(
  const exprt &expr1,
  const exprt &expr2,
  const namespacet &ns)
{
  base_type_eqt base_type_eq(ns);
  return base_type_eq.base_type_eq(expr1, expr2);
}
