#include "gcc-plugin.h"
#include "tree.h"
#include "stringpool.h"
#include "attribs.h"

#include "list.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define __unused __attribute__((unused))

#define DEFAULT_SEPARATOR  ("::")
#define DEFAULT_ATTRIBUTE  ("extract_offset")
#define DEFAULT_OUTPUT     ("/dev/stdout")
#define DEFAULT_CAPITALIZE (false)
#define DEFAULT_APPEND     (false)
#define DEFAULT_PREFIX     ("")

int plugin_is_GPL_compatible;

struct config {
        const char *match_attribute;
        const char *output_file;
        const char *separator;
        const char *prefix;
        bool capitalize;
        bool append;
};

struct data {
        struct config config;
        struct attribute_spec attr;
        struct list handled_records;
        FILE *outputf;
} DATA;

static bool should_export(tree decl)
{
        tree attr = lookup_attribute(DATA.config.match_attribute, DECL_ATTRIBUTES(decl));
        return (attr != NULL_TREE);
}

static bool struct_or_union(tree t)
{
        tree type = DECL_P(t) ? TREE_TYPE(t) : t;

        return (TREE_CODE(type) == RECORD_TYPE || TREE_CODE(type) == UNION_TYPE);
}

static bool is_anonymous(tree t)
{
        tree type = DECL_P(t) ? TREE_TYPE(t) : t;

        return (TYPE_IDENTIFIER(t) == NULL);
}

static const char *get_name_or_default(tree tnode, const char *def)
{
        gcc_assert(DECL_P(tnode) || TYPE_P(tnode));
        tree t;
        if (DECL_P(tnode)) {
                t = DECL_NAME(tnode);
        } else {
                t = TYPE_IDENTIFIER(tnode);
        }

        return (t != NULL ? IDENTIFIER_POINTER(t) : def);
}

static size_t get_field_bitoffset(tree field)
{
        tree offset = DECL_FIELD_OFFSET(field);
        tree bitoffset = DECL_FIELD_BIT_OFFSET(field);
        gcc_assert(TREE_CODE(offset) == INTEGER_CST);
        gcc_assert(TREE_CODE(bitoffset) == INTEGER_CST);

        // TODO: Check the length of HOST_WIDE_INT.
        size_t overall_offset = TREE_INT_CST_LOW(offset) * 8 + TREE_INT_CST_LOW(bitoffset);
        gcc_assert(TREE_INT_CST_LOW(offset) >= 0);
        gcc_assert(TREE_INT_CST_LOW(bitoffset) >= 0);

        STATIC_ASSERT(sizeof(TREE_INT_CST_LOW(offset)) >= sizeof((void *)0));

        return (overall_offset);
}

static char *strncpycap(char *dest, const char *src, size_t len)
{
        for (int i = 0; i < len; i++) {
                dest[i] = TOUPPER(src[i]);
        }
        return (dest);
}

static void save_offset(const char *cons_name, const char *field_name, size_t offset)
{
        const size_t slen = strlen(cons_name);
        const size_t flen = strlen(field_name);
        const size_t seplen = strlen(DATA.config.separator);
        // 1+ for null char.
        const size_t len = slen + flen + seplen + 1;

        char *fullname = (char *)xmalloc(len * sizeof(*fullname));
        gcc_assert(fullname);

        typedef char *(*cpyf)(char *dest, const char *src, size_t len);
        cpyf f = DATA.config.capitalize ? strncpycap : strncpy;

        f(&fullname[0], cons_name, slen);
        f(&fullname[slen], DATA.config.separator, seplen);
        f(&fullname[slen + seplen], field_name, flen);
        fullname[slen + seplen + flen] = '\0';
        gcc_assert(strlen(fullname) == len - 1);

        gcc_assert(offset % 8 == 0);
        fprintf(DATA.outputf, "%s%s %zu\n", DATA.config.prefix, fullname, offset / 8);

        free(fullname);
}

static void process_construct(tree construct, const char *parent_name, size_t base_offset)
{
        gcc_assert(struct_or_union(construct));

        if (list_contains(&DATA.handled_records, construct)) {
                return;
        }

        const char *cons_namestr = get_name_or_default(construct, parent_name);

        for (tree field = TYPE_FIELDS(construct); field != NULL; field = TREE_CHAIN(field)) {
                const char *field_namestr = get_name_or_default(field, NULL);
                const size_t field_offset = base_offset + get_field_bitoffset(field);

                if (should_export(field)) {
                        if (field_namestr != NULL) {
                                save_offset(cons_namestr, field_namestr, field_offset);
                        } else {
                                fprintf(stderr, "Can't export the anonymous construct without a name at %s:%d\n",
                                        DECL_SOURCE_FILE(field), DECL_SOURCE_LINE(field));
                        }
                }

                tree field_type = TREE_TYPE(field);
                // If it's not anonymous, we will handle it on another PLUGIN_FINISH_TYPE callback.
                if (struct_or_union(field_type) && is_anonymous(field_type)) {
                        // Some fields may be anonymous constructs, and don't have names, for example:
                        // struct some {
                        //         union {
                        //                 int64_t ax;
                        //                 struct {
                        //                         int32_t ah;
                        //                         int32_t al;
                        //                 };
                        //         };
                        // } s;
                        // In this example we can access the fields with as s.ax, s.ah and s.al.
                        // Therefore, there is no need to mention inner union and struct.

                        if (field_namestr == NULL) {
                                process_construct(field_type, cons_namestr, field_offset);
                        } else {
                                const size_t cons_namelen = strlen(cons_namestr);
                                const size_t fieldname_len = strlen(field_namestr);
                                const size_t separator_len = strlen(DATA.config.separator);

                                char *field_fullname = (char *)xmalloc(
                                        cons_namelen + fieldname_len + separator_len + 1);
                                strcpy(field_fullname, cons_namestr);
                                strcpy(&field_fullname[cons_namelen], DATA.config.separator);
                                strcpy(&field_fullname[cons_namelen + separator_len],
                                       field_namestr);

                                process_construct(field_type, field_fullname, field_offset);

                                free(field_fullname);
                        }
                }
        }

        list_add(&DATA.handled_records, construct);
}

static void process_type(void *gcc_data, void *user_data __unused)
{
        tree type = (tree)gcc_data;

        // Interested only in structs.
        if (!struct_or_union(type)) {
                return;
        }

        // Ignore anonymous structs. They will be handled as parts of parent structures.
        // There is a problem with global anonymous structures though, so...
        // TODO: Handle global anonymous structures.
        if (is_anonymous(type)) {
                return;
        }

        process_construct(type, NULL, 0);
}

static void handle_attributes(void *gcc_data __unused, void *user_data __unused)
{
        register_attribute(&DATA.attr);
}

static void handle_finish(void *gcc_data __unused, void *user_data __unused)
{
        list_destroy(&DATA.handled_records);
        fclose(DATA.outputf);
}

static struct config parse_args(int argc, struct plugin_argument *argv)
{
        struct config c = {
                .match_attribute = DEFAULT_ATTRIBUTE,
                .output_file = DEFAULT_OUTPUT,
                .separator = DEFAULT_SEPARATOR,
                .prefix = DEFAULT_PREFIX,
                .capitalize = DEFAULT_CAPITALIZE,
                .append = DEFAULT_APPEND,
        };

        for (int i = 0; i < argc; i++) {
                struct plugin_argument arg = argv[i];

#define argument_is(KEY) (strncmp(arg.key, (KEY), strlen(KEY)) == 0)

                if (argument_is("attribute")) {
                        c.match_attribute = arg.value;
                } else if (argument_is("output")) {
                        c.output_file = arg.value;
                } else if (argument_is("separator")) {
                        c.separator = arg.value;
                } else if (argument_is("capitalize")) {
                        c.capitalize = true;
                } else if (argument_is("prefix")) {
                        c.prefix = arg.value;
                } else if (argument_is("append")) {
                        c.append = true;
                } else {
                        fprintf(stderr, "Unknown argument: %s\n", arg.key);
                }
#undef argument_is
        }
        return (c);
}

int plugin_init(struct plugin_name_args *info, struct plugin_gcc_version *version __unused)
{
        // TODO: Version check
        DATA.config = parse_args(info->argc, info->argv);
        gcc_assert(DATA.config.output_file);

        const char *fmode = DATA.config.append ? "a" : "w";
        DATA.outputf = fopen(DATA.config.output_file, fmode);
        if (DATA.outputf == NULL) {
                perror("Couldn't open output file");
                return (EXIT_FAILURE);
        }

        DATA.attr = (struct attribute_spec){
                DATA.config.match_attribute, 0, 0, false, false, false, false, NULL
        };

        list_init(&DATA.handled_records);

        register_callback(info->base_name, PLUGIN_FINISH_TYPE, process_type, NULL);
        register_callback(info->base_name, PLUGIN_FINISH, handle_finish, NULL);
        register_callback(info->base_name, PLUGIN_ATTRIBUTES, handle_attributes, NULL);

        return (EXIT_SUCCESS);
}
