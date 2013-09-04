#include <string.h>
#include "vm.h"
#include "util.h"
#include "struct.h"
#include "serial.h"
#include "variable.h"

extern void mark_map(struct map *map, bool mark);
static void variable_value_str2(struct context *context, struct variable* v, char *str, size_t size);

#define ERROR_VAR_TYPE  "type error"

const struct number_string var_types[] = {
    {VAR_NIL,   "nil"},
    {VAR_INT,   "integer"},
    {VAR_FLT,   "float"},
    {VAR_STR,   "string"},
    {VAR_KVP,   "key-value-pair"},
    {VAR_LST,   "list"},
    {VAR_FNC,   "function"},
    {VAR_ERR,   "error"},
    {VAR_SRC,   "source"},
    {VAR_BOOL,  "boolean"},
    {VAR_CFNC,  "c-function"},
    {VAR_VOID,  "void"},
};

const char *var_type_str(enum VarType vt)
{
    return NUM_TO_STRING(var_types, vt);
}

struct variable* variable_new(struct context *context, enum VarType type)
{
    struct variable* v = (struct variable*)malloc(sizeof(struct variable));
    v->type = type;
    v->map = NULL;
    v->mark = 0;
    v->ptr = NULL;
    v->visited = VISITED_NOT;

    array_add(context->singleton->all_variables, v);
    //DEBUGPRINT("variable_new %s %p\n", var_type_str(type), v);
    return v;
}

struct variable *variable_new_void(struct context *context, void *p)
{
    struct variable *v = variable_new(context, VAR_VOID);
    v->ptr = p;
    return v;
}

struct variable* variable_new_nil(struct context *context) {
    return variable_new(context, VAR_NIL);
}

struct variable* variable_new_int(struct context *context, int32_t i)
{
    struct variable *v = variable_new(context, VAR_INT);
    v->integer = i;
    return v;
}

struct variable* variable_new_err(struct context *context, const char* message)
{
    struct variable *v = variable_new(context, VAR_ERR);
    v->str = byte_array_from_string(message);
    return v;
}

struct variable* variable_new_bool(struct context *context, bool b)
{
    struct variable *v = variable_new(context, VAR_BOOL);
    v->boolean = b;
    return v;
}

void variable_del(struct context *context, struct variable *v)
{
    //DEBUGPRINT("variable_del %p->%p %s\n", v, v->list, var_type_str(v->type));
    switch (v->type) {
        case VAR_CFNC:
        case VAR_NIL:
        case VAR_INT:
        case VAR_FLT:
        case VAR_KVP:
        case VAR_BOOL:
            break;
        case VAR_SRC:
        case VAR_LST:
            array_del(v->list);
            break;
        case VAR_STR:
        case VAR_FNC:
            byte_array_del(v->str);
            break;
        case VAR_VOID: // todo: I suppose I should do something here
            break;
        default:
            vm_exit_message(context, "bad var type");
            break;
    }
    if (NULL != v->map)
        map_del(v->map);

    free(v);
}

struct variable *variable_new_src(struct context *context, uint32_t size)
{
    struct variable *v = variable_new(context, VAR_SRC);
    v->list = array_new();

    while (size--) {
        struct variable *o = (struct variable*)stack_pop(context->operand_stack);
        if (o->type == VAR_SRC) {
            o->map = map_union(o->map, v->map);
            array_append(o->list, v->list);
            v = o;
        } else if (o->type == VAR_KVP)
            variable_map_insert(context, v, o->kvp.key, o->kvp.val);
        else
            array_insert(v->list, 0, o);
    }
//  DEBUGPRINT("src = %s\n", variable_value_str(context, v));
    v->list->current = v->list->data;
    return v;
}

struct variable *variable_new_bytes(struct context *context, struct byte_array *bytes, uint32_t size)
{
    struct variable *v = variable_new(context, VAR_BYT);
    v->str = bytes ? bytes : byte_array_new_size(size);
    return v;
}

struct variable* variable_new_float(struct context *context, float f)
{
    //DEBUGPRINT("new float %f\n", f);
    struct variable *v = variable_new(context, VAR_FLT);
    v->floater = f;
    return v;
}

struct variable *variable_new_str(struct context *context, struct byte_array *str) {
    struct variable *v = variable_new(context, VAR_STR);
    v->str = byte_array_copy(str);
    //DEBUGPRINT("variable_new_str %p->%s\n", v, byte_array_to_string(str));
    return v;
}

struct variable *variable_new_fnc(struct context *context, struct byte_array *body, struct variable *closures)
{
    struct variable *v = variable_new(context, VAR_FNC);
    v->str = byte_array_copy(body);
    v->map = closures ? map_copy(context, closures->map) : NULL;
    return v;
}

struct variable *variable_new_list(struct context *context, struct array *list)
{
    struct variable *v = variable_new(context, VAR_LST);
    v->list = array_new();
    //DEBUGPRINT("variable_new_list %p->%p\n", v, v->list);
    for (uint32_t i=0; list && (i<list->length); i++) {
        struct variable *u = (struct variable*)array_get(list, i);
        if (u->type == VAR_KVP) {
            v->map = map_union(v->map, u->map);
        } else
            array_set(v->list, v->list->length, u);
    }
    return v;
}

struct variable *variable_new_kvp(struct context *context, struct variable *key, struct variable *val)
{
    struct variable *v = variable_new(context, VAR_KVP);
    v->kvp.key = key;
    v->kvp.val = val;
    return v;
}

struct variable *variable_new_cfnc(struct context *context, callback2func *cfnc) {
    struct variable *v = variable_new(context, VAR_CFNC);
    v->cfnc = cfnc;
    return v;
}

// todo: check for buffer overruns more carefully
static void variable_value_strcat(struct context *context, char *str, struct variable *v, bool inkvp)
{
    int position = strlen(str);
    char *str2 = &str[position];
    int len2 = sizeof(str) - position;
    bool kk = (inkvp && (v->type == VAR_KVP));
    if (kk)
        strcat(str2, "(");
    variable_value_str2(context, v, str2, len2);
    if (kk)
        strcat(str2, ")");
}

static void variable_value_str2(struct context *context, struct variable* v, char *str, size_t size)
{
    assert_message(v && (v->visited < VISITED_LAST), "corrupt variable");
    null_check(v);
    
    enum VarType vt = (enum VarType)v->type;
    struct array* list = v->list;

    if (v->visited ==VISITED_MORE) { // first visit of reused variable
        sprintf(str, "&%d", v->mark);
        v->visited = VISITED_X;
    }
    else if (v->visited == VISITED_X) { // subsequent visit
        sprintf(str, "*%d", v->mark);
        return;
    }

    switch (vt) {
        case VAR_NIL:    sprintf(str, "%snil", str);                                break;
        case VAR_INT:    sprintf(str, "%s%d", str, v->integer);                     break;
        case VAR_BOOL:   sprintf(str, "%s%s", str, v->boolean ? "true" : "false");  break;
        case VAR_FLT:    sprintf(str, "%s%f", str, v->floater);                     break;
        case VAR_FNC:    sprintf(str, "%sf(%dB)", str, v->str->length);             break;
        case VAR_CFNC:   sprintf(str, "%sc-fnc", str);                              break;
        case VAR_VOID:   sprintf(str, "%s%p", str, v->ptr);                         break;
        case VAR_BYT:
            byte_array_print(str, VV_SIZE, v->str);
            break;
        case VAR_KVP:
            variable_value_strcat(context, str, v->kvp.key, true);
            strcat(str, ":");
            variable_value_strcat(context, str, v->kvp.val, true);
            break;
        case VAR_SRC:
        case VAR_LST: {
            strcat(str, "[");
            vm_null_check(context, list);
            for (int i=0; i<list->length; i++) {
                struct variable* element = (struct variable*)array_get(list, i);
                const char *c = i ? "," : "";
                sprintf(str, "%s%s", str, c);
                if (NULL != element)
                    variable_value_strcat(context, str, element, false);
            }
        } break;
        case VAR_STR: {
            char *str2 =  byte_array_to_string(v->str);
            sprintf(str, "%s'%s'", str, str2);
            free(str2);
        } break;
        case VAR_ERR: {
            char *str2 =  byte_array_to_string(v->str);
            strcpy(str, str2);
            free(str2);
        } break;
        default:
            vm_exit_message(context, ERROR_VAR_TYPE);
            break;
    }

    if (v->map)
    {
        struct array *keys = map_keys(v->map);
        struct array *vals = map_vals(v->map);

        for (int i=0; i<keys->length; i++)
        {
            if (v->list->length + i)
                strcat(str, ",");

            struct variable *key = (struct variable *)array_get(keys, i);
            struct variable *val = (struct variable *)array_get(vals, i);
            struct variable *kvp = variable_new_kvp(context, key, val);
            variable_value_strcat(context, str, kvp, false);

        } // for

        strcat(str, "]");

        array_del(keys);
        array_del(vals);

    } // map

    else if (vt == VAR_LST || vt == VAR_SRC)
        strcat(str, "]");
}

static void variable_mark2(struct variable *v, uint32_t *marker)
{
    if (VISITED_MORE == v->visited)
        return;
    if (VISITED_ONCE == v->visited) {
        v->visited = VISITED_MORE;
        return;
    }

    // first visit
    v->mark = ++(*marker);
    v->visited = VISITED_ONCE;

    if (VAR_LST == v->type)
    {
        for (int i=0; i<v->list->length; i++) {
            struct variable *v2 = (struct variable*)array_get(v->list, i);
            if (v2)
                variable_mark2(v2, marker);
        }
    }
    else if (VAR_KVP == v->type)
    {
        variable_mark2((struct variable*)v->kvp.key, marker);
        variable_mark2((struct variable*)v->kvp.val, marker);
    }

    //DEBUGPRINT("variable_mark2 %p->%p\n", v, v->map);

    if (VAR_LST == v->type)
        mark_map(v->map, true);
}

void variable_mark(struct variable *v)
{
    uint32_t marker = 0;
    variable_mark2(v, &marker);
}

void variable_unmark(struct variable *v)
{
    assert_message(v->type < VAR_LAST, "corrupt variable");
    if (VISITED_NOT == v->visited)
        return;
    v->mark = 0;
    v->visited = VISITED_NOT;

    if (v->type == VAR_LST) {
        for (int i=0; i<v->list->length; i++) {
            struct variable* element = (struct variable*)array_get(v->list, i);
            if (NULL != element)
                variable_unmark(element);
        }
    }

    mark_map(v->map, false);
}


char *variable_value_str(struct context *context, struct variable* v, char *buf)
{
    *buf = 0;
    variable_unmark(v);
    variable_mark(v);
    variable_value_str2(context, v, buf, sizeof(buf));
    variable_unmark(v);
    return buf;
}

struct byte_array *variable_value(struct context *c, struct variable *v) {
    char buf[VV_SIZE];
    const char *str = variable_value_str(c, v, buf);
    return byte_array_from_string(str);
}

struct variable *variable_pop(struct context *context)
{
    struct variable *v = (struct variable*)stack_pop(context->operand_stack);
    null_check(v);
//    DEBUGPRINT("\nvariable_pop %s\n", variable_value_str(context, v));
//    print_operand_stack(context);
    if (v->type == VAR_SRC) {
//        DEBUGPRINT("\tsrc %d ", v->list->length);
        if (v->list->length)
            v = (struct variable*)array_get(v->list, 0);
        else
            v = variable_new_nil(context);
    }
    return v;
}

void inline variable_push(struct context *context, struct variable *v) {
    stack_push(context->operand_stack, v);
}

struct byte_array *variable_serialize(struct context *context,
									  struct byte_array *bits,
                                      const struct variable *in)
{
	null_check(context);
    //DEBUGPRINT("\tserialize:%s\n", variable_value_str(context, (struct variable*)in));
    if (NULL == bits)
        bits = byte_array_new();
        serial_encode_int(bits, in->type);
    switch (in->type) {
        case VAR_INT:   serial_encode_int(bits, in->integer);   break;
        case VAR_FLT:   serial_encode_float(bits, in->floater); break;
        case VAR_BOOL:  serial_encode_int(bits, in->boolean);   break;
        case VAR_STR:
        case VAR_FNC:   serial_encode_string(bits, in->str);    break;
        case VAR_LST: {
            uint32_t len = in->list->length;
            serial_encode_int(bits, len);
            for (int i=0; i<len; i++) {
                const struct variable *item = (const struct variable*)array_get(in->list, i);
                variable_serialize(context, bits, item);
            }
        } break;
        case VAR_KVP:
            variable_serialize(context, bits, in->kvp.key);
            variable_serialize(context, bits, in->kvp.val);
            break;
        case VAR_NIL:
            break;
        default:
            vm_exit_message(context, "bad var type");
            break;
    }

    if (NULL != in->map) {
        struct array *keys = map_keys(in->map);
        struct array *values = map_vals(in->map);
        serial_encode_int(bits, keys->length);
        for (int i=0; i<keys->length; i++) {
            const struct variable *key = (const struct variable*)array_get(keys, i);
            const struct variable *value = (const struct variable*)array_get(values, i);
            variable_serialize(context, bits, key);
            variable_serialize(context, bits, value);
        }
        array_del(keys);
        array_del(values);
    } else
        serial_encode_int(bits, 0);

    return bits;
}

struct variable *variable_deserialize(struct context *context, struct byte_array *bits)
{
	null_check(context);
    assert_message(bits->length, "zero length serialized variable");
    struct variable *result = NULL;
    struct byte_array *str = NULL;

    enum VarType vt = (enum VarType)serial_decode_int(bits);
    switch (vt) {
        case VAR_NIL:
            result = variable_new_nil(context);
            break;
        case VAR_BOOL:
            result = variable_new_bool(context, serial_decode_int(bits));
            break;
        case VAR_INT:
            result = variable_new_int(context, serial_decode_int(bits));
            break;
        case VAR_FLT:
            result = variable_new_float(context, serial_decode_float(bits));
            break;
        case VAR_FNC:
            str = serial_decode_string(bits);
            result = variable_new_fnc(context, str, NULL);
            break;
        case VAR_STR:
            str = serial_decode_string(bits);
            result = variable_new_str(context, str);
            break;
        case VAR_BYT:
            str = serial_decode_string(bits);
            result =  variable_new_bytes(context, str, 0);
            break;
        case VAR_LST: {
            uint32_t len = serial_decode_int(bits);
            struct array *list = array_new_size(len);
            while (len--)
                array_add(list, variable_deserialize(context, bits));
            result = variable_new_list(context, list);
            array_del(list);
            break;
        case VAR_KVP: {
            struct variable *key = variable_deserialize(context, bits);
            struct variable *val = variable_deserialize(context, bits);
            result = variable_new_kvp(context, key, val);
            } break;
        }
        default:
            vm_exit_message(context, "bad var type");
    }

    uint32_t map_length = serial_decode_int(bits);
    if (map_length) {
        result->map = map_new(context);
        for (int i=0; i<map_length; i++) {
            struct variable *key = variable_deserialize(context, bits);
            struct variable *val = variable_deserialize(context, bits);
            map_insert(result->map, key, val);
        }
    }

    if (NULL != str)
        byte_array_del(str);
    return result;
}

uint32_t variable_length(struct context *context, const struct variable *v)
{
    switch (v->type) {
        case VAR_LST: return v->list->length;
        case VAR_STR: return v->str->length;
        case VAR_INT: return v->integer;
        case VAR_NIL: return 0;
        default:
            vm_exit_message(context, "non-indexable length");
            return 0;
    }
}

struct variable *variable_part(struct context *context, struct variable *self, uint32_t start, int32_t length)
{
    null_check(self);

    if (length < 0) // count back from end of list/string
        length = self->list->length + length + 1 - start;
    if (length < 0) // end < start
        length = 0;

    struct variable *result = NULL;
    switch (self->type) {
        case VAR_STR: {
            struct byte_array *str = byte_array_part(self->str, start, length);
            result = variable_new_str(context, str);
            byte_array_del(str);
            break;
        }
        case VAR_LST: {
            struct array *list = array_part(self->list, start, length);
            result = variable_new_list(context, list);
            array_del(list);
            break;
        }
        default:
            result = (struct variable*)exit_message("bad part type");
            break;
    }
    return result;
}

void variable_remove(struct variable *self, uint32_t start, int32_t length)
{
    null_check(self);
    switch (self->type) {
        case VAR_STR:
            byte_array_remove(self->str, start, length);
            break;
        case VAR_LST:
            array_remove(self->list, start, length);
            break;
        default:
            exit_message("bad remove type");
    }
}

struct variable *variable_concatenate(struct context *context, int n, const struct variable* v, ...)
{
    struct variable* result = variable_copy(context, v);

    va_list argp;
    for(va_start(argp, v); --n;) {
        struct variable* parameter = va_arg(argp, struct variable* );
        if (NULL == parameter)
            continue;
        else switch (result->type) {
            case VAR_STR: byte_array_append(result->str, parameter->str); break;
            case VAR_LST: array_append(result->list, parameter->list);    break;
            default: return (struct variable*)exit_message("bad concat type");
        }
    }

    va_end(argp);
    return result;
}

int variable_map_insert(struct context *context, struct variable* v, struct variable *key, struct variable *datum)
{
    //DEBUGPRINT("variable_map_insert into %p\n", v);
    assert_message(v->type != VAR_NIL, "can't insert into nil");
    if (NULL == v->map)
        v->map = map_new(context);
#ifdef DEBUG
    //char buf[VV_SIZE];
    //DEBUGPRINT("variable_map_insert %p %s into %p\n", datum, variable_value_str(context, datum, buf), v );
#endif

    // setting a value to nil means removing the key
    if (datum->type == VAR_NIL)
        return map_remove(v->map, key);
    return map_insert(v->map, key, datum);
}

struct variable *variable_map_get(struct context *context, struct variable *v, struct variable *key)
{
    assert_message(v->type == VAR_LST, "only lists may have maps");
    struct variable *result = lookup(context, v, key);
    if (result->type == VAR_SRC)
        result = array_get(result->list, 0);
    return result;
}

static bool variable_compare_maps(struct context *context, const struct map *umap, const struct map *vmap)
{
    if ((NULL == umap) && (NULL == vmap))
        return true;
    if (NULL == umap)
        return variable_compare_maps(context, vmap, umap);
    struct array *keys = map_keys(umap);
    bool result = true;
    if (NULL == vmap)
        result = !keys->length;
    else for (int i=0; i<keys->length; i++) {
        struct variable *key = (struct variable*)array_get(keys, i);
        struct variable *uvalue = (struct variable*)map_get(umap, key);
        struct variable *vvalue = (struct variable*)map_get(vmap, key);
        if (!variable_compare(context, uvalue, vvalue)) {
            result = false;
            break;
        }
    }
    array_del(keys);
    return result;
}

bool variable_compare(struct context *context, const struct variable *u, const struct variable *v)
{
    if ((NULL == u) != (NULL == v))
        return false;
    enum VarType ut = (enum VarType)u->type;
    enum VarType vt = (enum VarType)v->type;

    if (ut != vt)
        return false;

    switch (ut) {
        case VAR_LST:
            if (u->list->length != v->list->length)
                return false;
            for (int i=0; i<u->list->length; i++) {
                struct variable *ui = (struct variable*)array_get(u->list, i);
                struct variable *vi = (struct variable*)array_get(v->list, i);
                if (!variable_compare(context, ui, vi))
                    return false;
            }
            return variable_compare_maps(context, u->map, v->map);

        case VAR_BOOL:  return (u->boolean == v->boolean);
        case VAR_INT:   return (u->integer == v->integer);
        case VAR_FLT:   return (u->floater == v->floater);
        case VAR_STR:   return (byte_array_equals(u->str, v->str));
        case VAR_NIL:   return true;
        default:
            return (bool)vm_exit_message(context, "bad comparison");
    }
}

struct variable* variable_set(struct context *context, struct variable *dst, const struct variable* src)
{
    vm_null_check(context, dst);
    vm_null_check(context, src);
    switch (src->type) {
        case VAR_NIL:                                           break;
        case VAR_INT:   dst->integer = src->integer;            break;
        case VAR_FLT:   dst->floater = src->floater;            break;
        case VAR_FNC:
        case VAR_BYT:
        case VAR_STR:   dst->str = byte_array_copy(src->str);   break;
        case VAR_SRC:
        case VAR_LST:   dst->list = array_copy(src->list);      break;
        case VAR_KVP:   dst->kvp = src->kvp;                    break;
        case VAR_CFNC:  dst->cfnc = src->cfnc;                  break;
        case VAR_BOOL:  dst->boolean = src->boolean;            break;
        case VAR_VOID:  dst->ptr = src->ptr;                    break;
        default:
            vm_exit_message(context, "bad var type");
            break;
    }
    dst->map = map_copy(context, src->map);
    dst->type = src->type;
    return dst;
}

struct variable* variable_copy(struct context *context, const struct variable* v)
{
    //    DEBUGPRINT("variable_copy");
    vm_null_check(context, v);
    struct variable *u = variable_new(context, (enum VarType)v->type);
    variable_set(context, u, v);
    //DEBUGPRINT("variable_copy %p -> %p -> %p\n", context, v, u);
    return u;
}

struct variable *variable_find(struct context *context,
                               struct variable *self,
                               struct variable *sought,
                               struct variable *start)
{
    struct variable *result = NULL;
    
    if (self->type == VAR_STR && sought->type == VAR_STR) {                     // search for substring
        assert_message(!start || start->type == VAR_INT, "non-integer index");
        int32_t beginning = start ? start->integer : 0;
        int32_t index = byte_array_find(self->str, sought->str, beginning);
        if (index == -1)
            return variable_new_nil(context);
        return variable_new_int(context, index);
        
    }
    
    if (self->type == VAR_LST) {
        for (int i=0; !result && i<self->list->length; i++) {
            struct variable *v = (struct variable*)array_get(self->list, i);
            if ((sought->type == VAR_INT && v->type == VAR_INT && v->integer == sought->integer) ||
                (sought->type == VAR_STR && v->type == VAR_STR && byte_array_equals(sought->str, v->str)))
                return variable_new_int(context, i);
        }
    }

    return (NULL == result) ? variable_new_nil(context) : result;
}