#include <iostream>
#include <cctype>
#include <stack>
#include <map>
#include <sstream>
#include <vector>
#include <string>

extern "C" {
#include <stdio.h>
#include "aps-ag.h"
String get_code_name(Symbol);
}

#include "dump-scala.h"
#include "implement.h"

using namespace std;

using std::string;

// extra decl_flags flags:
#define IMPLEMENTED_PATTERN_VAR (1<<20)

extern int aps_yylineno;
int nesting_level = 0;

ostream& operator<<(ostream&o,Symbol s)
{
  String str = get_code_name(s);
  if (str == NULL) o << symbol_name(s);
  else o << get_code_name(s);
  return o;
}

ostream& operator<<(ostream&o,String s)
{
  if (s == NULL) {
    o << "<NULL>";
    return o;
  }
  int n = string_length(s);
  // char *buf = new char[n+1];
  char buf[n+1];
  realize_string(buf,s);
  return o << buf;
  // delete buf;
}

void print_uppercase(String sn,ostream&os)
{
  for (char *s=(char*)sn; *s; ++s) {
    if (islower(*s)) os<< (char)toupper(*s);
    else if (*s == '-') os << "_";
    else os << *s;
  }
}

// parameterizations and options:

static char* omitted[80];
static int omitted_number = 0;

void omit_declaration(char *n)
{
  omitted[omitted_number++] = n;
}

static char*impl_types[80];
static int impl_number = 0;

void impl_module(char *mname, char*type)
{
  impl_types[impl_number++] = mname;
  impl_types[impl_number++] = type;
}

bool incremental = false; //! unused
int verbose = 0;
int debug = 0;

int inline_definitions = 0;

void dump_scala_Program(Program p,std::ostream&oss)
{
  inline_definitions = 0;
  aps_yyfilename = (char *)program_name(p);
  oss << "import edu.uwm.cs.aps._;" << endl;
  static int implicit_num = 0;
  for (Unit u = first_Unit(program_units(p)); u; u = UNIT_NEXT(u)) {
    switch(Unit_KEY(u)) {
    case KEYno_unit: break;
    case KEYwith_unit: break;
    case KEYdecl_unit: 
      {
	Declaration d = decl_unit_decl(u);
	switch (Declaration_KEY(d)) {
	case KEYclass_decl:
	case KEYmodule_decl:
	case KEYpolymorphic:
	  dump_scala_Declaration(d,oss);
	  break;
	default:
	  // scala does not permit random things at the top level
	  oss << "object implicit_" << (++implicit_num) << "{\n";
	  ++nesting_level;
	  dump_scala_Declaration(d,oss);
	  oss << "}";
	  oss << "import implicit_" << implicit_num << "._\n";
	  break;
	}
      }
    }
  }
}

Declaration constructor_decl_base_type_decl(Declaration decl)
{
  Type t = constructor_decl_type(decl);
  Declaration returndecl = first_Declaration(function_type_return_values(t));
  Type return_type = value_decl_type(returndecl);
  Declaration tdecl = USE_DECL(type_use_use(return_type));
  return tdecl;
}

void dump_formal(Declaration formal, ostream&os)
{
  os << "v_" << decl_name(formal) << " : " << infer_formal_type(formal);
  if (KEYseq_formal == Declaration_KEY(formal)) os << "*";
}

void dump_function_prototype(string name, Type ft, ostream& oss)
{
  oss << indent() << "def v_" << name << "(";

  Declarations formals = function_type_formals(ft);
  for (Declaration formal = first_Declaration(formals);
       formal != NULL;
       formal = DECL_NEXT(formal)) {
    if (formal != first_Declaration(formals))
      oss << ", ";
    dump_formal(formal,oss);
  }
  oss << ")";

  Declaration returndecl = first_Declaration(function_type_return_values(ft));
  Type rt;
  if (returndecl == 0) {
    rt = 0;
    // oss << ":Unit";
  } else {
    rt = value_decl_type(returndecl);
    if (DECL_NEXT(returndecl)) {
      aps_error(ft,"cannot handle multiple return values");
    }
    oss << ":" << rt;
  }
}

void dump_pattern_prototype(string name, Type ft, ostream& oss)
{
  Declarations formals = function_type_formals(ft);
  Declaration first = first_Declaration(formals);

  oss << indent() << "val p_" << name;
  if (first) {
    oss << " : PatternFunction[(";
    for (Declaration f = first; f != NULL; f = DECL_NEXT(f)) {
      if (f != first) oss << ",";
      oss << infer_formal_type(f);
    }
    oss << "),";
  } else {
    oss << " : Pattern0Function[";
  }

  Declaration returndecl = first_Declaration(function_type_return_values(ft));
  Type rt = value_decl_type(returndecl);
  oss << rt << "]";
}

void dump_function_debug_start(char *name, Type ft, ostream& os)
{
  Declarations formals = function_type_formals(ft);
  os << indent() << "try {" << endl;
  ++nesting_level;
  os << indent() << "Debug.begin(\"" << name << "(\"";
  bool started = false;
  for (Declaration formal = first_Declaration(formals);
       formal != NULL;
       formal = DECL_NEXT(formal)) {
    if (started) os << "+\",\""; else started = true;
    os << "+v_" << decl_name(formal);
  }
  os << "+\")\");\n";
}

void dump_debug_end(ostream& os)
{
  --nesting_level;
  os << indent() << "} finally { Debug.end(); }" << endl;
}

// Output Scala pattern for APS pattern
void dump_Pattern(Pattern p, ostream& os);

void dump_Pattern(Pattern p, ostream& os) 
{
  switch (Pattern_KEY(p)) {
  default:
    aps_error(p,"Cannot implement this kind of pattern");
    break;

  case KEYmatch_pattern:
    switch (Pattern_KEY(match_pattern_pat(p))) {
    default:
      aps_error(p,"Cannot implement this kind of match pattern");
      break;
    case KEYpattern_var: break;
    }
    dump_Pattern(match_pattern_pat(p),os);
    os << ":";
    dump_Type(match_pattern_type(p),os);
    break;

  case KEYpattern_call:
    {
      Pattern pf = pattern_call_func(p);
      PatternActuals pactuals = pattern_call_actuals(p);
      Use pfuse;
      switch (Pattern_KEY(pf)) {
      default:
	aps_error(p,"cannot find constructor (can only handle ?x=f(...) or f(...)");
	return;
      case KEYpattern_use:
	pfuse = pattern_use_use(pf);
	break;
      }
      dump_Use(pfuse,"p_",os);
      bool first = true;
      for (Pattern pa = first_PatternActual(pactuals); pa ; pa = PAT_NEXT(pa)) {
	if (first) first = false; else os << ",";
	dump_Pattern(pa,os);
      }
    }
    break;

  case KEYrest_pattern:
    if (Pattern_KEY(rest_pattern_constraint(p)) != KEYno_pattern) {
      aps_error(p,"Cannot handle complicated ... patterns");
    }
    if (PAT_NEXT(p)) {
      aps_error(p,"Sequence pattern too complicated for now");
    } else {
      os << "_*";
    }

  case KEYand_pattern:
    // we handle some common cases
    // but in general APS's "and" pattern is too powerful for 
    // an easy translation to Scala
    switch (Pattern_KEY(and_pattern_p2(p))) {
    default: break;
    case KEYcondition:
      dump_Pattern(and_pattern_p1(p),os);
      os << " if ";
      dump_Expression(condition_e(and_pattern_p2(p)),os);
      return;
    }
    switch (Pattern_KEY(and_pattern_p1(p))) {
    default:
      aps_error(p,"Cannot implement this kind of and pattern");
      break;
    case KEYpattern_var: break;
    }
    dump_Pattern(and_pattern_p1(p),os);
    os <<"@";
    dump_Pattern(and_pattern_p2(p),os);
    break;

  case KEYpattern_var:
    {
      Declaration f = pattern_var_formal(p);
      os << "v_" << def_name(formal_def(f));
    }
    break;
  }
}


bool type_is_syntax(Type t)
{
  switch (Type_KEY(t)) {
  case KEYno_type: 
  case KEYremote_type:
  case KEYprivate_type:
  case KEYfunction_type:
    return false;
  case KEYtype_use:
    {
      Declaration d = USE_DECL(type_use_use(t));
      switch (Declaration_KEY(d)) {
      case KEYphylum_formal:
      case KEYphylum_decl:
	return true;
      case KEYtype_formal:
      case KEYtype_decl:
	return false;
      case KEYtype_renaming:
	return type_is_syntax(type_renaming_old(d));
      default:
	aps_error(d,"What sort of type_decl is this?");
      }
    }
  case KEYtype_inst:
    {
      Module m = type_inst_module(t);
      switch (Module_KEY(m)) {
      default:
	aps_error(m,"What sort of module is this?");
      case KEYmodule_use: 
	{
	  Declaration d = USE_DECL(module_use_use(m));
	  switch (Declaration_KEY(d)) {
	  default:
	    aps_error(m,"What sort of module is this?");
	  case KEYmodule_decl:
	    switch (Declaration_KEY(module_decl_result_type(d))) {
	    default:
	      aps_error(m,"What sort of result decl does this module have?");
	    case KEYtype_decl: return false;
	    case KEYphylum_decl: return true;
	    }
	  }
	}
      }
    }
  }
  return false;
}


static Declaration find_basic_decl(string name)
{
  Program p = find_Program(make_string("basic"));
  Units us = program_units(p);
  for (Unit u = first_Unit(us); u; u = UNIT_NEXT(u)) {
    switch (Unit_KEY(u)) {
    default: break;
    case KEYdecl_unit:
      {
	Declaration d = decl_unit_decl(u);
	if (decl_namespaces(d) > 0 && name == decl_name(d))
	  return d;
      }
      break;
    }
  }
  aps_error(p,"Couldn't find basic declaration of %s" ,name.c_str());
  return 0;
}

// Currently inheritances does the transfer of values,
// but we need this to do the transfer of types:

class ServiceRecord : public map<Symbol,int> {
public:
  void add(Declaration d) {
    int namespaces = decl_namespaces(d);
    if (namespaces) {
      (*this)[def_name(declaration_def(d))] |= namespaces;
    }
  }
  int missing(Declaration d) {
    if (int namespaces = decl_namespaces(d)) {
      return namespaces & ~(*this)[def_name(declaration_def(d))];
    }
    return 0;
  }
};

void dump_Signature_service_transfers(ServiceRecord&,string,Signature,
				      ostream&);

void dump_Class_service_transfers(ServiceRecord& sr, string from,
				  Declaration cd, ostream& oss)
{
  Declarations ds = block_body(some_class_decl_contents(cd));
  for (Declaration d = first_Declaration(ds); d; d = DECL_NEXT(d)) {
    if (sr.missing(d)) {
      string n = decl_name(d);
      sr.add(d);
      switch (Declaration_KEY(d)) {
      default: break;
      case KEYvalue_decl:
	oss << indent() << "val v_" << n
	    << " = t_" << from << ".v_" << n << ";\n";
	break;
      case KEYpattern_decl:
      case KEYconstructor_decl:
	oss << indent() << "val p_" << n
	    << " = t_" << from << ".p_" << n << ";\n";
	if (Declaration_KEY(d) == KEYpattern_decl) break;
	/* fall through */
      case KEYfunction_decl:
      case KEYattribute_decl:
	{
	  Type ft = some_function_decl_type(d);
	  Declarations fs = function_type_formals(ft);
	  dump_function_prototype(n,ft,oss);
	  oss << " = t_" << from << ".f_" << n << "(";
	  bool started = false;
	  for (Declaration f = first_Declaration(fs); f; f=DECL_NEXT(f)) {
	    if (started) oss << ",";
	    else started = true;
	    oss << "v_" << decl_name(f);
	  }
	  oss << ";\n";
	}
	break;
      case KEYtype_decl:
      case KEYphylum_decl:
      case KEYtype_renaming:
	oss << indent() << "type T_" << n
	    << " = t_" << from << ".T_" << n << ";\n";
	oss << indent() << "val t_" << n
	    << " = t_" << from << ".t_" << n << ";\n";	
	break;
      }
    }
  }
}

void dump_Signature_service_transfers(ServiceRecord& sr, string from,
				      Signature s, ostream& oss)
{
  switch (Signature_KEY(s)) {
  case KEYno_sig:
    break;
  case KEYsig_use:
    {
      Use u = sig_use_use(s);
      Declaration d = USE_DECL(u);
      switch (Declaration_KEY(d)) {
      case KEYsignature_decl:
	dump_Signature_service_transfers(sr,from,
					 sig_subst(u,signature_decl_sig(d)),
					 oss);
	break;
      case KEYsignature_renaming:
	s = signature_renaming_old(d);
	dump_Signature_service_transfers(sr,from,sig_subst(u,s),oss);
      default:
	// There shouldn't be any other possibilities
	aps_error(d,"unexpected signature decl");
      }
    }
    break;
  case KEYfixed_sig:
    break;
  case KEYmult_sig:
    dump_Signature_service_transfers(sr,from,mult_sig_sig1(s),oss);
    dump_Signature_service_transfers(sr,from,mult_sig_sig2(s),oss);
    break;
  case KEYsig_inst:
    {
      Declaration cd = USE_DECL(some_use_u(sig_inst_class(s)));
      dump_Class_service_transfers(sr,from,cd,oss);
    }
  }
}

void dump_Type_service_transfers(ServiceRecord& sr,
				 string from,
				 bool is_phylum,
				 Type ty, ostream& oss)
{
  switch (Type_KEY(ty)) {
  case KEYno_type:
    {
      Declaration cd = find_basic_decl(is_phylum ? "PHYLUM" : "TYPE");
      dump_Class_service_transfers(sr,from,cd,oss);
    }
    break;
  case KEYremote_type:
    // set is_phylum to true because we want nodes.
    dump_Type_service_transfers(sr,from,true,remote_type_nodetype(ty),oss);
    break;
  case KEYprivate_type:
    dump_Type_service_transfers(sr,from,is_phylum,private_type_rep(ty),oss);
    break;
  case KEYfunction_type:
    // do nothing
    break;
  case KEYtype_inst:
    {
      Module m = type_inst_module(ty);
      switch (Module_KEY(m)) {
      case KEYmodule_use:
	dump_Class_service_transfers(sr,from,USE_DECL(module_use_use(m)),oss);
	break;
      }
    }
    break;
  case KEYtype_use:
    {
      Use u = type_use_use(ty);
      Type as = 0;
      Declaration td = USE_DECL(u);
      string name = decl_name(td);
      switch (Declaration_KEY(td)) {
      case KEYtype_decl:
	as = type_subst(u,type_decl_type(td));
	dump_Type_service_transfers(sr,name,false,as,oss);
	break;
      case KEYphylum_decl:
	as = type_subst(u,phylum_decl_type(td));
	dump_Type_service_transfers(sr,name,true,as,oss);
	break;
      case KEYphylum_formal:
	is_phylum = true;
	// fall through
      case KEYtype_formal:
	{
	  Signature sig = sig_subst(u,some_type_formal_sig(td));
	  dump_Signature_service_transfers(sr,name,sig,oss);
	}
	break;
      case KEYtype_renaming:
	dump_Type_service_transfers(sr,name,is_phylum,
				    type_subst(u,type_renaming_old(td)),oss);
	break;
      default:
	aps_error(td,"What sort of type decl to get services from ?");
      }
    }
    break;
  }
}

void dump_some_attribute(Declaration d,
			 Type nt, Type vt,
			 Direction dir,
			 Default deft,
			 Implementation::ModuleInfo *info,
			 ostream& oss)
{
  char *name = decl_name(d);
  bool is_col = direction_is_collection(dir);
  bool is_cir = direction_is_circular(dir);

  ostringstream vtt;
  vtt << vt;

  oss << indent() << "private object a_" << name << " extends Attribute["
      << nt << "," << vt << "](nt,vt,\"" << name << "\")"
      << (is_cir ? " with Circular[" + vtt.str() + "]" : "") 
      << (is_col ? " with Collection[" + vtt.str() + "]" : "")
      << " {\n";
  ++nesting_level;

  switch (Default_KEY(deft)) {
  default:
    break;
  case KEYsimple:
    oss << indent() << "override def getDefault(n : " << nt << ") = "
	<< simple_value(deft) << ";\n";
    break;
  case KEYcomposite:
    oss << indent() << "override def getDefault(n : " << nt << ") = "
	<< composite_initial(deft) << ";\n";
    oss << indent() << "override def combine(v1 : " << vt
	<< ", v2 : " << vt << ") = ";
    oss << composite_combiner(deft) << "(v1,v2);\n";
  }

  if (Declaration_KEY(d) == KEYvalue_decl) {
    info->note_local_attribute(d,oss);
  } else {
    info->note_attribute_decl(d,oss);
  }

  --nesting_level;
  oss << indent() << "}\n";

}
  
void dump_local_attributes(Block b, Type at, Implementation::ModuleInfo* info,
			   ostream& oss)
{
  for (Declaration d = first_Declaration(block_body(b)); d; d=DECL_NEXT(d)) {
    switch (Declaration_KEY(d)) {
    default:
      aps_error(d,"Cannot handle this kind of statement");
      break;
    case KEYvalue_decl:
      {
	dump_some_attribute(d,at,value_decl_type(d),
			    value_decl_direction(d),
			    value_decl_default(d),info,oss);
      }
      break;
    case KEYassign:
      break;
    case KEYblock_stmt:
      dump_local_attributes(block_stmt_body(d),at,info,oss);
      break;
    case KEYif_stmt:
      dump_local_attributes(if_stmt_if_true(d),at,info,oss);
      dump_local_attributes(if_stmt_if_false(d),at,info,oss);
      break;
    case KEYcase_stmt:
      {
	FOR_SEQUENCE
	  (Match,m,Matches,case_stmt_matchers(d),
	   dump_local_attributes(matcher_body(m),at,info,oss));

	dump_local_attributes(case_stmt_default(d),at,info,oss);
      }
      break;
    }
  }
}

static void dump_TypeFormal(Declaration tf, ostream& os) {
  os << "T_" << decl_name(tf);
}

void dump_TypeFormals(TypeFormals tfs, ostream& os)
{
  bool started = false;
  for (Declaration tf=first_Declaration(tfs); tf ; tf = DECL_NEXT(tf)) {
    if (started) os << ", ";
    else { started = true; os << "["; }
    dump_TypeFormal(tf,os);
  }
  if (started) os << "]";
}

void dump_TypeFormal_value(Declaration tf, ostream& os) {
  os << "t_" << decl_name(tf) << ":";
  ostringstream ss;
  ss << "T_" << decl_name(tf);
  dump_Signature(some_type_formal_sig(tf),ss.str(),os);
}

char* decl_code_name(Declaration decl)
{    
  char *name = (char*)get_code_name(def_name(declaration_def(decl)));
  if (!name) name = decl_name(decl);
  return name;
}

void dump_some_class_decl(Declaration decl, ostream& oss)
{
  oss << indent() << "trait C_" << def_name(some_class_decl_def(decl))
      << "[T_Result";
  TypeFormals tfs = some_class_decl_type_formals(decl);
  for (Declaration tf=first_Declaration(tfs); tf ; tf = DECL_NEXT(tf)) {
    oss << ", ";
    dump_TypeFormal(tf,oss);
  }
  oss << "] extends ";
  dump_Signature(some_class_decl_parent(decl),"T_Result",oss);
  oss << "{\n";
  ++nesting_level;
  Declarations body = block_body(some_class_decl_contents(decl));
  for (Declaration d=first_Declaration(body); d; d=DECL_NEXT(d)) {
    char *n = decl_code_name(d);
    switch (Declaration_KEY(d)) {
    default:
      aps_warning(d,"nested thing not handled in APS class");
      break;
    case KEYphylum_decl:
    case KEYtype_decl:
    case KEYtype_renaming:
      oss << indent() << "type T_" << n << ";\n";
    case KEYvalue_decl:
      oss << indent() << "val v_" << n << " : " 
	  << value_decl_type(d) << ";\n";
      break;
    case KEYattribute_decl:
    case KEYfunction_decl:
      dump_function_prototype(n,some_function_decl_type(d),oss);
      oss << ";\n";
      break;
    case KEYconstructor_decl:
      dump_function_prototype(n,constructor_decl_type(d),oss);
      oss << ";\n";
      dump_pattern_prototype(n,constructor_decl_type(d),oss);
      oss << ";\n";
      break;
    case KEYpattern_decl:
      dump_pattern_prototype(n,constructor_decl_type(d),oss);
      oss << ";\n";
      break;
    case KEYpragma_call:
    case KEYtop_level_match:
      break;
    }
  }
  --nesting_level;
  oss << indent() << "}\n";
}

static void dump_new_type(string n, ostream& oss)
{
  oss << indent() << "class T_" << n << " extends Type {\n"
      << indent() << "  def getType() = t_" << n << ";\n"
      << indent() << "}\n\n"
      << indent() << "object t_" << n << " extends I_Type[T_" << n << "] {}\n"; 
}

static void dump_new_phylum(string n, ostream& oss)
{
  oss << indent() << "class T_" << n << " extends Phylum {\n"
      << indent() << "  def getType() = t_" << n << ";\n"
      << indent() << "}\n\n"
      << indent() << "object t_" << n << " extends I_PHYLUM[T_" << n << "] {\n" 
      << indent() << "  def isComplete : Boolean = complete;\n"
      << indent() << "}\n";
}

static void dump_type_inst(string n, Type ti, ostream& oss)
{
  Module m = type_inst_module(ti);
  TypeActuals tas = type_inst_type_actuals(ti);
  Actuals as = type_inst_actuals(ti);
  char *rname = "Result";
  int u=0;
  for (Type ta = first_TypeActual(tas); ta ; ta = TYPE_NEXT(ta)) {
    switch (Type_KEY(ta)) {
    default: break;
    case KEYtype_inst: 
      {
	ostringstream ss;
	ss << n << ++u;
	dump_type_inst(ss.str(),ta,oss);
	break;
      }
    }
  }
  u=0;
  oss << indent() << "m_" << n << " = new ";
  switch (Module_KEY(m)) {
  default:
    aps_error(m,"cannot handle this module");
    break;
  case KEYmodule_use:
    rname = decl_name(module_decl_result_type(USE_DECL(module_use_use(m))));
    oss << "M_" << symbol_name(use_name(module_use_use(m)));
    break;
  }
  bool started = false;
  for (Type ta = first_TypeActual(tas); ta ; ta = TYPE_NEXT(ta)) {
    if (started) oss << ",";
    else {
      oss << "[";
      started = true;
    }
    switch (Type_KEY(ta)) {
    default:
      oss << ta;
      break;
    case KEYtype_inst: 
      {
	oss << "T_" << n << u;
	break;
      }
    }
  }
  if (started) oss << "]";
  started = false;

  for (Type ta = first_TypeActual(tas); ta ; ta = TYPE_NEXT(ta)) {
    if (started) oss << ",";
    else {
      oss << "(";
      started = true;
    }
    switch (Type_KEY(ta)) {
    default:
      oss << as_val(ta);
      break;
    case KEYtype_inst: 
      {
	oss << "t_" << n << u;
	break;
      }
    }
  }
  for (Expression a = first_Actual(as); a; a = EXPR_NEXT(a)) {
    if (started) oss << ",";
    else {
      oss << "(";
      started = true;
    }
    oss << a;
  }
  if (started) oss << ")";
  oss << ";\n";
  oss << indent() << "type T_" << n << " = m_" << n << ".T_" << rname;
  oss << indent() << "val t_" << n << " = m_" << n << ".t_" << rname;
}

void dump_scala_Declaration(Declaration decl,ostream& oss)
{
  char *name = 0;
  switch (Declaration_KEY(decl)) {
  case KEYdeclaration:
    name = (char*)get_code_name(def_name(declaration_def(decl)));
    if (!name) name = decl_name(decl);
    break;
  default:
    break;
  }
  if (name)
    for (int i=0; i < omitted_number; ++i)
      if (streq(omitted[i],name)) return;

  switch (Declaration_KEY(decl)) {
  case KEYclass_decl: 
    dump_some_class_decl(decl,oss);
    break;
  case KEYmodule_decl:
    {
      Declarations body = block_body(module_decl_contents(decl));
      Declaration rdecl = module_decl_result_type(decl);
      char *rname = decl_name(rdecl);
      bool rdecl_is_phylum = (Declaration_KEY(rdecl) == KEYphylum_decl);
      DECL_NEXT(rdecl) = first_Declaration(body);
      Declaration first_decl = rdecl;
      char *impl_type = 0;

      for (int j=0; j < impl_number; ++j)
	if (streq(impl_types[j],name)) impl_type = impl_types[j+1];
      if (impl_type) first_decl = first_Declaration(body);
      
      dump_some_class_decl(decl,oss);
      
      oss << indent() << "class M_" << def_name(some_class_decl_def(decl));
      TypeFormals tfs = some_class_decl_type_formals(decl);
      dump_TypeFormals(tfs,oss);
      {
	bool started = false;
	for (Declaration tf=first_Declaration(tfs); tf ; tf = DECL_NEXT(tf)) {
	  if (started) oss << ",";
	  else { started = true; oss << "("; }
	  dump_TypeFormal_value(tf,oss);
	}
	Formals vfs = module_decl_value_formals(decl);
	for (Declaration vf=first_Declaration(vfs); vf; vf = DECL_NEXT(vf)) {
	  if (started) oss << ",";
	  else { started = true; oss << "("; }
	  dump_formal(vf,oss);
	  if (started) oss << ")";
	}
      }

      oss << " {\n";
      ++nesting_level;

      Type rut = some_type_decl_type(rdecl);
      char *source = "tmp";

      // define T_Result:
      if (impl_type) {
	oss << indent() << "type T_" << rname << " = " 
	    << impl_type << ";\n";
      } else {
	switch (Type_KEY(rut)) {
	case KEYno_type:
	  if (rdecl_is_phylum) {
	    dump_new_phylum(source,oss);
	  } else {
	    dump_new_type(source,oss);
	  }
	  break;
	case KEYtype_inst:
	  dump_type_inst(source,rut,oss);
	  break;
	default:
	  oss << indent() << "type T_" << source << " = " << rut << ";\n";
	  oss << indent() << "val t_" << source << " = " << as_val(rut) <<";\n";
	}
	oss << indent() << "type T_" << rname
	    << " = T_" << source << ";\n";
      }

      // define t_Result
      oss << indent() << "object t_" << rname
	  << " extends C_" << name << "[" << "T_" << rname;
      for (Declaration tf=first_Declaration(tfs); tf ; tf = DECL_NEXT(tf)) {
	oss << ",T_" << decl_name(tf);
      }
      oss << "] {\n";
      ++nesting_level;

      ServiceRecord sr;
      // get "inherited" services:
      // need to get typedefs which don't inherit
      for (Declaration d = first_decl; d ; d = DECL_NEXT(d)) {
	sr.add(d);
      }
      dump_Type_service_transfers(sr,source,rdecl_is_phylum,rut,oss);

      Implementation::ModuleInfo *info = impl->get_module_info(decl);

      for (Declaration d = first_decl; d ; d = DECL_NEXT(d)) {
	string n = decl_name(d);
	dump_scala_Declaration(d,oss);
      
	// now specific to module things:
	switch (Declaration_KEY(d)) {
	case KEYattribute_decl:
	  {
	    Type fty = attribute_decl_type(d);
	    Declaration f = first_Declaration(function_type_formals(fty));
	    Declarations rdecls = function_type_return_values(fty);
	    Declaration rdecl = first_Declaration(rdecls);

	    dump_some_attribute(d,infer_formal_type(f),
				value_decl_type(rdecl),
				attribute_decl_direction(d),
				attribute_decl_default(d),info,oss);

	    oss << indent() << "def v_" << n
		<< "(anode : " << infer_formal_type(f) << ") : "
		<< value_decl_type(rdecl)
		<< " = a_" << n << ".evaluate(anode);\n";
	  }
	  break;
	case KEYtop_level_match:
	  {
	    Match m = top_level_match_m(d);
	    Type anchor_type = infer_pattern_type(matcher_pat(m));
	    Block body = matcher_body(m);
	    dump_local_attributes(body,anchor_type,info,oss);
	  }
	  info->note_top_level_match(d,oss);
	  break;
	case KEYvalue_decl:
	  {
	    Def def = value_decl_def(d);
	    if (!def_is_constant(def))
	      info->note_var_value_decl(d,oss);
	  }
	  break;
	default:
	  break;
	}
      }

      info->implement(oss);

      --nesting_level;
      oss << indent() << "}\n";
      --nesting_level;
      oss << indent() << "}\n";
    }
    break;

  case KEYtype_decl:
  case KEYphylum_decl:
    {
      bool is_phylum = (KEYphylum_decl == Declaration_KEY(decl));
      Type type = some_type_decl_type(decl);
      switch (Type_KEY(type)) {
      case KEYno_type:
	if (is_phylum) {
	  dump_new_type(name,oss);
	} else {
	  dump_new_phylum(name,oss);
	}
	break;
      case KEYtype_inst:
	dump_type_inst(name,type,oss);
	break;
      default:
	oss << indent() << "type T_" << name << " = " << type << ";\n";
	oss << indent() << "val t_" << name << " = " << as_val(type) << ";\n";
	break;
      }
    }
    break;

  case KEYconstructor_decl:
    {
      Type ft = constructor_decl_type(decl);
      Declarations formals = function_type_formals(ft);
      Declaration tdecl = constructor_decl_base_type_decl(decl);
      Declarations rdecls = function_type_return_values(ft);
      Type rt = value_decl_type(first_Declaration(rdecls));
      bool is_syntax = false;
      // char *base_type_name = decl_name(tdecl);
      switch (Declaration_KEY(tdecl)) {
      case KEYphylum_decl:
	is_syntax = true;
	break;
      case KEYtype_decl:
	is_syntax = false;
	break;
      default:
	aps_error(decl,"constructor not built on type or phylum");
	return;
      }

      // the case class:
      oss << indent() << "case class c_" << name << "(";
      bool started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	if (started) oss << ","; else started = true;
	dump_formal(f,oss);
      }
      oss << ") extends " << rt << "{\n";
      ++nesting_level;
      oss << indent() << "def children : List[Phylum] = List(";
      started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	Type fty = formal_type(f);
	if (type_is_syntax(fty)) {
	  if (started) oss << ","; else started = true;
	  oss << "v_" << decl_name(f);
	}
      }
      oss << ");\n";
      --nesting_level;
      oss << "};\n";

      // helper: "(v_a1,v_a2)"
      ostringstream args;
      started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	if (started) args << ","; else started = true;
	args << "v_" << decl_name(f);
      }
      args << ")";

      // helper: "(T_A1,T_A2)"
      ostringstream typess;
      started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	if (started) typess << ","; else started = true;
	typess << infer_formal_type(f);
      }
      typess << ")";
      string types = started ? typess.str() : "Unit";
      
      // the constructor function:
      dump_function_prototype(name,ft,oss);
      oss << " = c_" << name << args.str() << ";\n";

      // the unconstructor function:
      oss << indent() << "def u_" << name << "(x:" << rt << ") : ";
      oss << " : Option[" << types << "] = x match {\n";
      oss << indent() << "  case c_" << name << args.str()
	  << " => Some(" << args.str() << ");\n";
      oss << indent() << "  case _ => None };\n";

      // the pattern function
      oss << indent() << "  val p_" << name;
      if (types == "Unit") {
	oss << " = new Pattern0Function[";
      } else {
	oss << " = new PatternFunction[" << types << ",";
      }
      oss << rt << "](u_" << name << ");\n";
      
    }
    break;

  case KEYvalue_decl:
    {
      oss << indent() << "var v_" << name << ":" << value_decl_type(decl);
      switch (Default_KEY(value_decl_default(decl))) {
      case KEYsimple:
	oss << " = ";
	dump_Expression(simple_value(value_decl_default(decl)),oss);
	break;
      case KEYno_default:
	aps_error(decl,"No default value?");
	break;
      default:
	aps_error(decl,"Cannot generate code for this");
	break;
      }
      oss << ";\n";
    }
    break;

  case KEYattribute_decl:
    // must be handled by the surrounding module
    break;

  case KEYfunction_decl:
    {
      Type fty = function_decl_type(decl);
      Declaration rdecl = first_Declaration(function_type_return_values(fty));
      Block b = function_decl_body(decl);
      dump_function_prototype(name,fty,oss);

      // three kinds of definitions:
      // 1. the whole thing: a non-empty body:
      if (first_Declaration(block_body(b))) {
	if (!inline_definitions) {
	  dump_function_prototype(name,fty,oss);
	}
	oss << " = {\n";
	++nesting_level;
	if (debug)
	  dump_function_debug_start(name,fty,oss);
	impl->implement_function_body(decl,oss);
	if (debug)
	  dump_debug_end(oss);
	--nesting_level;
	oss << indent() << "}\n" << endl;
	return;
      } else if (rdecl) {
	// 2. simple default
	switch (Default_KEY(value_decl_default(rdecl))) {
	case KEYsimple:
	  {
	    Expression result = simple_value(value_decl_default(rdecl));
	    oss << " = ";
	    if (debug) {
	      oss << "{\n";
	      ++nesting_level;
	      dump_function_debug_start(decl_name(decl),fty,oss);
	      oss << indent() << "return " << result << ";\n";
	      dump_debug_end(oss);
	      --nesting_level;
	      oss << indent() << "};\n";
	    } else {
	      oss << result << ";\n";
	    }
	    return;
	  }
	default:
	  break;
	}
      }
      // cout << name << " has no body.\n";
      // 3. nothing -- leave to native code
      oss << ";\n";
    }
    break;

  case KEYpolymorphic:
    {
      Declarations tfs = polymorphic_type_formals(decl);
      Declarations body = block_body(polymorphic_body(decl));
      oss << indent() << "class M_" << name << "[";
      bool started = false;
      for (Declaration f=first_Declaration(tfs); f; f=DECL_NEXT(f)) {
	if (started) oss << ","; else started = true;
	oss << "T_" << decl_name(f);
      }
      oss << "](";
      started = false;
      for (Declaration f=first_Declaration(tfs); f; f=DECL_NEXT(f)) {
	if (started) oss << ","; else started = true;
	dump_TypeFormal_value(f,oss);
      }
      oss << ") {\n";
      ++nesting_level;
      for (Declaration d=first_Declaration(body); d; d=DECL_NEXT(d)) {
	dump_scala_Declaration(d,oss);
      }
      --nesting_level;      
      oss << "};\n\n";
    }
    break;
  case KEYtop_level_match:
    break;
  case KEYvalue_renaming:
    {
      Expression old = value_renaming_old(decl);
      Type ty = infer_expr_type(old);
      if (Type_KEY(ty) == KEYfunction_type) {
	dump_function_prototype(name,ty,oss);
	oss << " = ";
	aps_yylineno = tnode_line_number(decl);
	Actuals as = nil_Actuals();
	for (Declaration f=first_Declaration(function_type_formals(ty));
	     f ; f = DECL_NEXT(f)) {
	  Use u = use(def_name(declaration_def(f)));
	  USE_DECL(u) = f;
	  as = append_Actuals(as,list_Actuals(value_use(u)));
	}
	dump_Expression(funcall(old,as),oss);
	oss << ";\n";
      } else {
	oss << indent() << "val v_" << name << " : " << ty << " = ";
	dump_Expression(old,oss);
	oss << ";\n" << endl;
      }
    }
    break;
  case KEYtype_renaming:
    {
      Type old = type_renaming_old(decl);
      oss << indent() << "type T_" << name << " = " << old << ";\n";
      oss << indent() << "val t_" << name << " = " << as_val(old) << ";\n";
    }
    break;
  case KEYpragma_call:
    break;
  case KEYpattern_decl:
    {
      Type ft = pattern_decl_type(decl);
      Declarations formals = function_type_formals(ft);
      Declarations rdecls = function_type_return_values(ft);
      Type rt = value_decl_type(first_Declaration(rdecls));
      Patterns choices = choice_pattern_choices(pattern_decl_choices(decl));
      
      // helper: "(v_a1,v_a2)"
      ostringstream args;
      bool started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	if (started) args << ","; else started = true;
	args << "v_" << decl_name(f);
      }
      args << ")";

      // helper: "(T_A1,T_A2)"
      ostringstream typess;
      started = false;
      for (Declaration f = first_Declaration(formals); f; f = DECL_NEXT(f)) {
	if (started) typess << ","; else started = true;
	typess << formal_type(f);
      }
      typess << ")";
      string types = started ? typess.str() : "Unit";

      // the unconstructor function:
      oss << indent() << "def u_" << name << "(x:" << rt << ") : ";
      oss << " : Option[" << types << "] = x match {\n";
      for (Pattern p = first_Pattern(choices); p; p=PAT_NEXT(p)) {
	oss << indent() << "  case ";
	dump_Pattern(p,oss);
	oss << " => Some(" << args.str() << ");\n";
      }
      oss << indent() << "  case _ => None }\n;";

      // the pattern function
      oss << indent() << "  val p_" << name;
      if (types == "Unit") {
	oss << " = new Pattern0Function[";
      } else {
	oss << " = new PatternFunction[" << types << ",";
      }
      oss << rt << "](u_" << name << ");\n";	
    }
    break;
  default:
    cout << "Not handling declaration " << decl_name(decl) << endl;
  }
}

bool no_type_is_phylum(Type ty) 
{
  Declaration decl = (Declaration)tnode_parent(ty);
  if (ABSTRACT_APS_tnode_phylum(decl) == KEYDeclaration) {
    switch (Declaration_KEY(decl)) {
    case KEYtype_decl:
      return false;
    case KEYphylum_decl:
      return true;
    default:
      break;
    }
  }
  aps_error(decl,"no_type occurs in a strange place");
  return false;
}

// use a sloppy global variable to avoid rewriting code
static int type_prefix = 0;
void dump_Type_prefixed(Type t, ostream&o)
{
  ++type_prefix;
  dump_Type(t,o);
  --type_prefix;
}

void dump_Signature(Signature s, string n, ostream& o)
{
  switch (Signature_KEY(s)) {
  case KEYsig_use:
    {
      Declaration d = USE_DECL(sig_use_use(s));
      switch (Declaration_KEY(d)) {
      default:
	aps_error(s,"cannot handle this signature decl");
	break;
      case KEYsignature_decl:
	dump_Signature(signature_decl_sig(d),n,o);
	break;
      }
    }
    break;
  case KEYsig_inst:
    {
      Class c = sig_inst_class(s);
      o << "C_" << decl_name(USE_DECL(class_use_use(c))) << "[" << n;
      TypeActuals tas = sig_inst_actuals(s);
      for (Type ta = first_TypeActual(tas); ta; ta=TYPE_NEXT(ta)) {
	o << "," << ta;
      }
      o << "]";
    }
    break;
  case KEYmult_sig:
    dump_Signature(mult_sig_sig1(s),n,o);
    o << " with "; //! BUG: what if one is a no_sig ot a fixed sig?
    dump_Signature(mult_sig_sig2(s),n,o);
    break;
  case KEYno_sig:
  case KEYfixed_sig:
    break;    
  }
}

void dump_Type(Type t, ostream& o)
{
  switch (Type_KEY(t)) {
  default:
    aps_error(t,"can't dump type of this nature");
    break;
  case KEYtype_use:
    {
      Use u = type_use_use(t);
      switch (Use_KEY(u)) {
      case KEYuse:
	o << "T_" << symbol_name(use_name(u));
	break;
      case KEYqual_use:
	o << qual_use_from(u) << ".T_" << symbol_name(qual_use_name(u));
	break;
      }
    }
    break;
  case KEYremote_type:
    dump_Type(remote_type_nodetype(t),o);
    break;
  case KEYprivate_type:
    dump_Type(private_type_rep(t),o);
    break;
  case KEYfunction_type:
    {
      o << "(";
      bool started = false;
      for (Declaration f=first_Declaration(function_type_formals(t));
	   f ; f = DECL_NEXT(f)) {
	if (started) o << ","; else started = true;
	dump_Type(formal_type(f),o);
	if (Declaration_KEY(f) == KEYseq_formal)
	  o << "*";
      }
      Declaration rdecl = first_Declaration(function_type_return_values(t));
      dump_Type(value_decl_type(rdecl),o);
      o << ") =>" << value_decl_type(rdecl);
    }
    break;
  }
}

void dump_Type_value(Type t, ostream& o)
{
  switch (Type_KEY(t)) {
  default:
    aps_error(t,"can't dump type of this nature");
    break;
  case KEYtype_use:
    {
      Use u = type_use_use(t);
      switch (Use_KEY(u)) {
      case KEYuse:
	o << "t_" << symbol_name(use_name(u));
	break;
      case KEYqual_use:
	o << qual_use_from(u) << ".t_" << symbol_name(qual_use_name(u));
	break;
      }
    }
    break;
  case KEYremote_type:
    dump_Type_value(remote_type_nodetype(t),o);
    break;
  case KEYprivate_type:
    dump_Type_value(private_type_rep(t),o);
    break;
  case KEYfunction_type:
    o << "new C_NULL[" << t << "]"; // no services
    break;
  }
}

void dump_vd_Default(Declaration d, ostream& o)
{
  Type vt = value_decl_type(d);
  Direction dir = value_decl_direction(d);
  Default deft = value_decl_default(d);
  switch (Default_KEY(deft)) {
  case KEYsimple:
    dump_Expression(simple_value(deft),o);
    break;
  case KEYcomposite:
    dump_Expression(composite_initial(deft),o);
    break;
  default:
    if (direction_is_collection(dir)) {
      o << as_val(vt) << ".v_initial";
    } else if (direction_is_circular(dir)) {
      o << as_val(vt) << ".v_bottom";
    } else {
      /*? print something ?*/
      o << "??";
    }
    break;
  }
}

bool funcall_is_collection_construction(Expression e)
{
  static SYMBOL braces = intern_symbol("{}");

  switch (Expression_KEY(funcall_f(e))) {
  default:
    break;
  case KEYvalue_use:
    {
      Use u = value_use_use(funcall_f(e));
      Symbol sym = 0;
      switch (Use_KEY(u)) {
      case KEYuse:
	sym = use_name(u);
	break;
      case KEYqual_use:
	sym = qual_use_name(u);
	break;
      }
      if (sym == braces) return true;
    }
    break;
  }
  return false;
}

void dump_collect_Actuals(Type ctype, Actuals as, ostream& o)
{
  switch (Actuals_KEY(as)) {
  case KEYnil_Actuals:
    dump_Type_value(ctype,o);
    o << ".v_none()";
    break;
  case KEYlist_Actuals:
    {
      Expression e = list_Actuals_elem(as);
      /* Handle a special case of sequence expression: */
      if (Expression_KEY(e) == KEYrepeat) {
	Expression seq = repeat_expr(e);
	if (base_type_equal(infer_expr_type(seq),ctype)) {
	  dump_Expression(seq,o);
	  return;
	}
	/* Fall through and cause error */
      }
      dump_Type_value(ctype,o);
      o << ".v_single(";
      dump_Expression(list_Actuals_elem(as),o);
      o << ")";
    }
    break;
  case KEYappend_Actuals:
    dump_Type_value(ctype,o);
    o << ".v_append(";
    dump_collect_Actuals(ctype,append_Actuals_l1(as),o);
    o << ",";
    dump_collect_Actuals(ctype,append_Actuals_l2(as),o);
    o << ")";
  }
}

void dump_Expression(Expression e, ostream& o)
{
  switch (Expression_KEY(e)) {
  case KEYinteger_const:
    o << integer_const_token(e);
    break;
  case KEYreal_const:
    o << real_const_token(e);
    break;
  case KEYstring_const:
    o << string_const_token(e);
    break;
  case KEYfuncall:
    if (funcall_is_collection_construction(e)) {
      // inline code to call append, single and null constructors
      dump_collect_Actuals(infer_expr_type(e),funcall_actuals(e),o);
      return;
    }
    dump_Expression(funcall_f(e),o);
    o << "(";
    {
      bool start = true;
      FOR_SEQUENCE(Expression,arg,Actuals,funcall_actuals(e),
		   if (start) start = false;
		   else o << ",";
		   dump_Expression(arg,o));
      Declarations fs = function_type_formals(infer_expr_type(funcall_f(e)));
      for (Declaration f=first_Declaration(fs); f; f=DECL_NEXT(f))
	if (Declaration_KEY(f) == KEYseq_formal) {
	  if (start) start = false; else o << ",";
	  o << "0";
	}
    }
    o << ")";
    break;
  case KEYvalue_use:
    {
      Use u = value_use_use(e);
      Declaration d = USE_DECL(u);
      if (Declaration_info(d)->decl_flags & IMPLEMENTATION_MARKS) {
	impl->implement_value_use(d,o);
      } else if (Declaration_info(d)->decl_flags & IMPLEMENTED_PATTERN_VAR) {
	o << *((string*)(Pattern_info((Pattern)tnode_parent(d))->pat_impl));
      } else {
	dump_Use(u,"v_",o);
      }
    }
    break;
  case KEYtyped_value:
    dump_Expression(typed_value_expr(e),o);
    break;
  default:
    aps_error(e,"cannot handle this kind of expression");
  }
}

static void dump_TypeContour(TypeContour *tc, bool instance, ostream& os) 
{
  os << "M_";
  os << decl_name(tc->source);
  vector<Type> type_actuals;
  switch (Declaration_KEY(tc->source)) {
  case KEYpolymorphic:
    {
      int n=0;
      for (Declaration f=first_Declaration(tc->type_formals);
	   f; f=DECL_NEXT(f))
	++n;
      for (int i=0; i < n; ++i) {
	type_actuals.push_back(tc->u.inferred[i]);
      }
    }
    break;
  case KEYmodule_decl:
    for (Type ta = first_TypeActual(tc->u.type_actuals);
	 ta ; ta = TYPE_NEXT(ta)) {
      type_actuals.push_back(ta);
    }
    break;
  default:
    fatal_error("%d: Not a source", tnode_line_number(tc->source));
  }

  bool started = false;
  for (unsigned i=0; i < type_actuals.size(); ++i) {
    if (started) os << ","; else { os << "[ "; started = true;}
    if (type_actuals[i] == 0) {
      fatal_error("insufficient type inference");
    }
    dump_Type(type_actuals[i],os);
  }
  if (started) os << "]";
  if (instance) {
    started = false;
    for (unsigned i=0; i < type_actuals.size(); ++i) {
      if (started) os << ","; else { os << "("; started = true;}
      dump_Type_value(type_actuals[i],os);
    }
    if (started) os << ")";
  }
}

static void dump_poly_inst(Use u, TypeEnvironment type_env, ostream& os)
{
  if (type_env == 0) return;
  if (!type_env->source || Declaration_KEY(type_env->source) != KEYpolymorphic) return;
  dump_poly_inst(u,type_env->outer,os);
  dump_TypeContour(type_env,true,os);
  os << ".";
}

void dump_Use(Use u, char *prefix, ostream& os)
{
  Symbol sym;
  switch (Use_KEY(u)) {
  case KEYuse:
    sym = use_name(u);
    break;
  case KEYqual_use:
    {
      dump_Type_value(qual_use_from(u),os);
      os << ".";
      sym = qual_use_name(u);
      break;
    }
  }
  dump_poly_inst(u,USE_TYPE_ENV(u),os);
  os << prefix << sym;
}

void debug_fiber(FIBER fiber, ostream& os) {
  while (fiber != NULL && fiber->field != NULL) {
    if (FIELD_DECL_IS_REVERSE(fiber->field)) {
      os << "X" << decl_name(reverse_field(fiber->field));
    } else {
      os << decl_name(fiber->field);
    }
    fiber = fiber->shorter;
    if (fiber->field != NULL) os << "_";
  }
}

void debug_fibered_attr(FIBERED_ATTRIBUTE* fa, ostream& os)
{
  if (ATTR_DECL_IS_SHARED_INFO(fa->attr))
    os << "__global";
  else
    os << decl_name(fa->attr);
  if (fa->fiber == 0) return;
  os << "$";
  debug_fiber(fa->fiber,os);
}

void debug_Instance(INSTANCE *i, ostream& os) {
  if (i->node != NULL) {
    if (ABSTRACT_APS_tnode_phylum(i->node) != KEYDeclaration) {
      os << tnode_line_number(i->node) << ":?<" 
	 << ABSTRACT_APS_tnode_phylum(i->node) << ">";
    } else if (Declaration_KEY(i->node) == KEYnormal_assign) {
      Declaration pdecl = proc_call_p(normal_assign_rhs(i->node));
      os << decl_name(pdecl) << "@";
    } else if (Declaration_KEY(i->node) == KEYpragma_call) {
      os << pragma_call_name(i->node) << "(...):" 
	 << tnode_line_number(i->node);
    } else {
      os << symbol_name(def_name(declaration_def(i->node)));
    }
    os << ".";
  }
  if (i->fibered_attr.attr == NULL) {
    os << "(nil)";
  } else if (ABSTRACT_APS_tnode_phylum(i->fibered_attr.attr) == KEYMatch) {
    os << "<match@" << tnode_line_number(i->fibered_attr.attr) << ">";
  } else switch(Declaration_KEY(i->fibered_attr.attr)) {
  case KEYcollect_assign: {
    Expression lhs = collect_assign_lhs(i->node);
    Declaration field = field_ref_p(lhs);
    os << "<collect[" << decl_name(field) << "]@"
       << tnode_line_number(i->fibered_attr.attr) << ">";
  }
  case KEYif_stmt:
  case KEYcase_stmt:
    os << "<cond@" << tnode_line_number(i->fibered_attr.attr) << ">";
    break;
  default:
    os << symbol_name(def_name(declaration_def(i->fibered_attr.attr)));
  }
  if (i->fibered_attr.fiber != NULL) {
    os << "$";
    debug_fiber(i->fibered_attr.fiber,os);
  }
}

string operator+(string s, int i)
{
  ostringstream os;
  os << s << i;
  return os.str();
}

string indent(int nl) { return string(indent_multiple*nl,' '); }

