// Copyright (c) 2018, Peter Ohler, All rights reserved.

#include <stdlib.h>

#include <ruby.h>
#include <ruby/thread.h>

#include "err.h"
#include "gqleval.h"
#include "gqlvalue.h"
#include "graphql.h"
#include "sdl.h"

static VALUE	graphql_class = Qundef;

typedef struct _eval {
    gqlDoc	doc;
    agooErr	err;
    gqlValue	value;
} *Eval;

static void
make_ruby_use(VALUE root, const char *method, const char *type_name) {
    struct _agooErr	err = AGOO_ERR_INIT;
    gqlType		type;
    gqlDirUse		use;
    volatile VALUE	v = rb_funcall(root, rb_intern(method), 0);

    if (Qnil == v) {
	return;
    }
    if (NULL == (type = gql_type_get(type_name))) {
	rb_raise(rb_eStandardError, "Failed to find the '%s' type.", type_name);
    }
    if (NULL == (use = gql_dir_use_create(&err, "ruby"))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (AGOO_ERR_OK != gql_dir_use_arg(&err, use, "class", gql_string_create(&err, rb_obj_classname(v), 0))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    gql_type_directive_use(type, use);
}

static VALUE
rescue_error(VALUE x) {
    Eval		eval = (Eval)x;
    volatile VALUE	info = rb_errinfo();
    volatile VALUE	msg = rb_funcall(info, rb_intern("message"), 0);
    const char		*classname = rb_obj_classname(info);
    const char		*ms = rb_string_value_ptr(&msg);

    agoo_err_set(eval->err, AGOO_ERR_EVAL, "%s: %s", classname, ms);

    return Qfalse;
}

static VALUE
call_eval(void *x) {
    Eval	eval = (Eval)x;

    volatile VALUE	foo = rb_funcall((VALUE)gql_root, rb_intern("to_s"), 0);
    const char		*str = rb_string_value_ptr(&foo);

    printf("*** call eval -- %s\n", str);

    eval->value = gql_doc_eval(eval->err, eval->doc);

    return Qnil;
}

static void*
protect_eval(void *x) {
    rb_rescue2(call_eval, (VALUE)x, rescue_error, (VALUE)x, rb_eException, 0);

    return NULL;
}

static gqlValue
eval_wrap(agooErr err, gqlDoc doc) {
    struct _eval	eval = {
	.doc = doc,
	.err = err,
	.value = NULL,
    };
    
    rb_thread_call_with_gvl(protect_eval, &eval);

    return eval.value;
}

static VALUE
gval_to_ruby(gqlValue value) {

    // TBD

    return Qnil;
}

static gqlRef
resolve(agooErr err, gqlRef target, const char *field_name, gqlKeyVal args) {
    VALUE	rargs[16];
    VALUE	*a = rargs;
    int		cnt = 0;
    volatile VALUE	v;
    volatile VALUE	result;

    if (NULL != args) {
	for (; NULL != args->key; args++, a++) {
	    *a = gval_to_ruby(args->value);
	}
    }
    // TBD args
    v = rb_funcall((VALUE)target, rb_intern("to_s"), 0);
    printf("*** resolve %s.%s\n", rb_string_value_ptr(&v), field_name);

    result = rb_funcallv((VALUE)target, rb_intern(field_name), cnt, rargs);

    v = rb_funcall((VALUE)result, rb_intern("to_s"), 0);

    return (gqlRef)result;
}

static VALUE
ref_to_string(gqlRef ref) {
    volatile VALUE	value;
    
    if (T_STRING == rb_type((VALUE)ref)) {
	value = (VALUE)ref;
    } else {
	value = rb_funcall((VALUE)ref, rb_intern("to_s"), 0);
    }
    return value;
}

static gqlValue
coerce(agooErr err, gqlRef ref, gqlType type) {
    gqlValue	value = NULL;
    
    if (NULL == type) {
	// This is really an error but make a best effort anyway.

	// TBD
    } else if (GQL_SCALAR != type->kind) {
	rb_raise(rb_eStandardError, "Can not coerce a non-scalar into a %s.", type->name);
    } else { // TBD a scalar?
	volatile VALUE	v;
	
	if (&gql_int_type == type) {
	    value = gql_int_create(err, RB_NUM2INT(rb_to_int((VALUE)ref)));
	} else if (&gql_bool_type == type) {
	    if (Qtrue == (VALUE)ref) {
		value = gql_bool_create(err, true);
	    } else if (Qfalse == (VALUE)ref) {
		value = gql_bool_create(err, false);
	    } else {
		// TBD int, float, string
	    }
	} else if (&gql_float_type == type) {
	    value = gql_float_create(err, rb_num2dbl(rb_to_float((VALUE)ref)));
	} else if (&gql_time_type == type) {
	    // TBD
	} else if (&gql_uuid_type == type) {
	    // TBD
	} else if (&gql_url_type == type) {
	    // TBD
	} else if (&gql_string_type == type) {
	    v = ref_to_string(ref);
	    value = gql_string_create(err, rb_string_value_ptr(&v), RSTRING_LEN(v));
	} else if (&gql_i64_type == type) {
	    value = gql_i64_create(err, RB_NUM2LONG(rb_to_int((VALUE)ref)));
	} else if (&gql_token_type == type) {
	    v = ref_to_string(ref);
	    value = gql_token_create(err, rb_string_value_ptr(&v), RSTRING_LEN(v));
	}
	if (NULL == value) {
	    if (AGOO_ERR_OK == err->code) {
		agoo_err_set(err, AGOO_ERR_EVAL, "Failed to coerce a %s into a %s.", rb_obj_classname((VALUE)ref), type->name);
	    }
	}
    }
    return value;
}

/* Document-method: schema
 *
 * call-seq: schema(root) { }
 *
 * Start the GraphQL/Ruby integration by assigning a root Ruby object to the
 * GraphQL environment. Within the block passed to the function SDL strings or
 * files can be loaded. On exiting the block validation of the loaded schema
 * is performed.
 *
 * Note that the _@ruby_ directive is added to the _schema_ type as well as
 * the _Query_, _Mutation_, and _Subscriptio_ type based on the _root_
 * class. Any _@ruby_ directives on the object types in the SDL will also
 * associate a GraphQL and Ruby class. The association will be used to
 * validate the Ruby class to verify it implements yhe methods described by
 * the GraphQL type.
 */
static VALUE
graphql_schema(VALUE self, VALUE root) {
    struct _agooErr	err = AGOO_ERR_INIT;
    gqlType		type;
    gqlDir		dir;
    gqlDirUse		use;
    
    if (!rb_block_given_p()) {
	rb_raise(rb_eStandardError, "A block is required.");
    }
    if (AGOO_ERR_OK != gql_init(&err)) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (NULL == (dir = gql_directive_create(&err, "ruby", "Associates a Ruby class with a GraphQL type.", 0))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (NULL == gql_dir_arg(&err, dir, "class", &gql_string_type, NULL, 0, NULL, true)) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (AGOO_ERR_OK != gql_directive_on(&err, dir, "SCHEMA", 6) ||
	AGOO_ERR_OK != gql_directive_on(&err, dir, "OBJECT", 6)) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    gql_root = (gqlRef)root;
    rb_gc_register_address(&root);

    gql_doc_eval_func = eval_wrap;
    gql_resolve_func = resolve;
    gql_coerce_func = coerce;

    if (NULL == (use = gql_dir_use_create(&err, "ruby"))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (AGOO_ERR_OK != gql_dir_use_arg(&err, use, "class", gql_string_create(&err, rb_obj_classname(root), 0))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    if (NULL == (type = gql_type_get("schema"))) {
	rb_raise(rb_eStandardError, "Failed to find the 'schema' type.");
    }
    gql_type_directive_use(type, use);

    make_ruby_use(root, "query", "Query");
    make_ruby_use(root, "mutation", "Mutation");
    make_ruby_use(root, "subscription", "Subscription");

    if (rb_block_given_p()) {
	rb_yield_values2(0, NULL);
    }
    if (AGOO_ERR_OK != gql_validate(&err)) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    return Qnil;
}

/* Document-method: load
 *
 * call-seq: load(sdl)
 *
 * Load an SDL string. This should only be called in a block provided to a
 * call to _#schema_.
 */
static VALUE
graphql_load(VALUE self, VALUE sdl) {
    struct _agooErr	err = AGOO_ERR_INIT;

    if (NULL == gql_root) {
	rb_raise(rb_eStandardError, "GraphQL root not set. Use Agoo::GraphQL.schema.");
    }
    rb_check_type(sdl, T_STRING);
    if (AGOO_ERR_OK != sdl_parse(&err, StringValuePtr(sdl), RSTRING_LEN(sdl))) {
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    return Qnil;
}

/* Document-method: load_file
 *
 * call-seq: load_file(sdl_file)
 *
 * Load an SDL file. This should only be called in a block provided to a call
 * to _#schema_.
 */
static VALUE
graphql_load_file(VALUE self, VALUE path) {
    struct _agooErr	err = AGOO_ERR_INIT;
    FILE		*f;
    size_t		len;
    char		*sdl;
    
    if (NULL == gql_root) {
	rb_raise(rb_eStandardError, "GraphQL root not set. Use Agoo::GraphQL.schema.");
    }
    rb_check_type(path, T_STRING);
    if (NULL == (f = fopen(StringValuePtr(path), "r"))) {
	rb_raise(rb_eIOError, "%s", strerror(errno));
    }
    if (0 != fseek(f, 0, SEEK_END)) {
	rb_raise(rb_eIOError, "%s", strerror(errno));
    }
    len = ftell(f);
    sdl = ALLOC_N(char, len + 1);
    fseek(f, 0, SEEK_SET);
    if (len != fread(sdl, 1, len, f)) {
	rb_raise(rb_eIOError, "%s", strerror(errno));
    } else {
	sdl[len] = '\0';
    }
    fclose(f);
    if (AGOO_ERR_OK != sdl_parse(&err, sdl, len)) {
	xfree(sdl);
	rb_raise(rb_eStandardError, "%s", err.msg);
    }
    xfree(sdl);

    return Qnil;
}

/* Document-method: dump_sdl
 *
 * call-seq: dump_sdl()
 *
 * The preferred method of inspecting a GraphQL schema is to use introspection
 * queries but for debugging and for reviewing the schema a dump of the schema
 * as SDL can be helpful. The _#dump_sdl_ method returns the schema as an SDL
 * string.
 *
 * - *options* [_Hash_] server options
 *
 *   - *:with_description* [_true_|_false_] if true the description strings are included. If false they are not included.
 *
 *   - *:all* [_true_|_false_] if true all types and directives are included in the dump. If false only the user created types are include.
 *
 */
static VALUE
graphql_sdl_dump(VALUE self, VALUE options) {
    agooText		t = agoo_text_allocate(4096);
    volatile VALUE	dump;
    VALUE		v;
    bool		with_desc = true;
    bool		all = false;
    
    Check_Type(options, T_HASH);

    v = rb_hash_aref(options, ID2SYM(rb_intern("with_descriptions")));
    if (Qnil != v) {
	with_desc = (Qtrue == v);
    }
    v = rb_hash_aref(options, ID2SYM(rb_intern("all")));
    if (Qnil != v) {
	all = (Qtrue == v);
    }
    t = gql_schema_sdl(t, with_desc, all);

    dump = rb_str_new(t->text, t->len);
    agoo_text_release(t);
    
    return dump;
}

/* Document-class: Agoo::Graphql
 *
 * The Agoo::GraphQL class provides support for the GraphQL API as defined in
 * https://facebook.github.io/graphql/June2018. The approach taken supporting
 * GraphQL with Ruby is to keep the integration as simple as possible. With
 * that in mind there are not new languages or syntax to learn. GraphQL types
 * are defined with SDL which is the language used in the specification. Ruby,
 * is well, just Ruby. A GraphQL type is assigned a Ruby class that implements
 * that type. Thats it. A GraphQL directive or a Ruby method is used to create
 * this association. After that the Agoo server does the work and calls the
 * Ruby object methods as needed to satisfy the GraphQL queries.
 */
void
graphql_init(VALUE mod) {
    graphql_class = rb_define_class_under(mod, "GraphQL", rb_cObject);

    rb_define_module_function(graphql_class, "schema", graphql_schema, 1);

    rb_define_module_function(graphql_class, "load", graphql_load, 1);
    rb_define_module_function(graphql_class, "load_file", graphql_load_file, 1);

    rb_define_module_function(graphql_class, "sdl_dump", graphql_sdl_dump, 1);
}