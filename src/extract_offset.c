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
#define DEFAULT_PREFIX     ("")

int plugin_is_GPL_compatible;

struct config {
        const char *match_attribute;
        const char *output_file;
        const char *separator;
        const char *prefix;
        bool capitalize;
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

static size_t get_field_offset(tree field)
{
        tree offset = DECL_FIELD_OFFSET(field);
        tree bitoffset = DECL_FIELD_BIT_OFFSET(field);
        gcc_assert(TREE_CODE(offset) == INTEGER_CST);
        gcc_assert(TREE_CODE(bitoffset) == INTEGER_CST);

        // TODO: Check the length of HOST_WIDE_INT.
        size_t overall_offset = TREE_INT_CST_LOW(offset) + TREE_INT_CST_LOW(bitoffset);
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

static void save_offset(const char *struct_name, const char *field_name, size_t offset)
{
        const size_t slen = strlen(struct_name);
        const size_t flen = strlen(field_name);
        const size_t seplen = strlen(DATA.config.separator);
        // 1+ for null char.
        const size_t len = slen + flen + seplen + 1;

        char *fullname = (char *)xmalloc(len * sizeof(*fullname));
        gcc_assert(fullname);

        typedef char *(*cpyf)(char *dest, const char *src, size_t len);
        cpyf f = DATA.config.capitalize ? strncpycap : strncpy;

        f(&fullname[0], struct_name, slen);
        f(&fullname[slen], DATA.config.separator, seplen);
        f(&fullname[slen + seplen], field_name, flen);
        fullname[slen + seplen + flen] = '\0';
        gcc_assert(strlen(fullname) == len - 1);

        gcc_assert(offset % 8 == 0);
        fprintf(DATA.outputf, "%s%s %zu\n", DATA.config.prefix, fullname, offset / 8);
}

static void handle_struct_type(tree decl, const char *parent_name, size_t base_offset)
{
        gcc_assert(TREE_CODE(decl) == RECORD_TYPE);

        if (list_contains(&DATA.handled_records, decl)) {
                return;
        }

        tree struct_id = TYPE_IDENTIFIER(decl);
        // Anonymous structures don't have type-names.
        const char *struct_name = (struct_id != NULL) ? IDENTIFIER_POINTER(struct_id) : parent_name;

        for (tree field = TYPE_FIELDS(decl); field != NULL; field = TREE_CHAIN(field)) {
                const char *field_name = IDENTIFIER_POINTER(DECL_NAME(field));
                const size_t field_offset = base_offset + get_field_offset(field);

                if (should_export(field)) {
                        save_offset(struct_name, field_name, field_offset);
                }

                tree field_type = TREE_TYPE(field);
                bool is_structure = TREE_CODE(field_type) == RECORD_TYPE;
                // If it's not anonymous, we will handle it on another PLUGIN_FINISH_TYPE callback.
                bool field_struct_anon = TYPE_IDENTIFIER(field_type) == NULL;
                if (is_structure && field_struct_anon) {
                        const size_t prename_len = strlen(struct_name);
                        const size_t fieldname_len = strlen(field_name);
                        const size_t separator_len = strlen(DATA.config.separator);

                        char *struct_prefix =
                                (char *)xmalloc(prename_len + fieldname_len + separator_len);
                        strcpy(struct_prefix, struct_name);
                        strcpy(&struct_prefix[prename_len], DATA.config.separator);
                        strcpy(&struct_prefix[prename_len + separator_len], field_name);

                        handle_struct_type(field_type, struct_prefix, field_offset);

                        free(struct_prefix);
                }
        }

        list_add(&DATA.handled_records, decl);
}

static void handle_attributes(void *gcc_data __unused, void *user_data __unused)
{
        register_attribute(&DATA.attr);
}

static void handle_finish_type(void *gcc_data, void *user_data __unused)
{
        tree type_decl = (tree)gcc_data;

        // Interested only in structs.
        if (TREE_CODE(type_decl) != RECORD_TYPE) {
                return;
        }

        // Ignore anonymous structs. They will be handled as parts of parent structures.
        // There is a problem with global anonymous structures though, so...
        // TODO: Handle global anonymous structures.
        bool anonymous_struct = TYPE_IDENTIFIER(type_decl) == NULL;
        if (anonymous_struct) {
                return;
        }

        handle_struct_type(type_decl, NULL, 0);
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

        DATA.outputf = fopen(DATA.config.output_file, "w+");
        if (DATA.outputf == NULL) {
                perror("Couldn't open output file");
                return (EXIT_FAILURE);
        }

        DATA.attr = (struct attribute_spec){
                DATA.config.match_attribute, 0, 0, false, false, false, false, NULL
        };

        list_init(&DATA.handled_records);

        register_callback(info->base_name, PLUGIN_FINISH, handle_finish, NULL);
        register_callback(info->base_name, PLUGIN_FINISH_TYPE, handle_finish_type, NULL);
        register_callback(info->base_name, PLUGIN_ATTRIBUTES, handle_attributes, NULL);

        return (EXIT_SUCCESS);
}
