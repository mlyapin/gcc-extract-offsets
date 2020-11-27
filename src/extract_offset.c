#include "gcc-plugin.h"
#include "tree.h"
#include "stringpool.h"
#include "attribs.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define __unused __attribute__((unused))

#define DEFAULT_SEPARATOR  ("_")
#define DEFAULT_ATTRIBUTE  ("extract_offset")
#define DEFAULT_OUTPUT     ("/dev/stdout")
#define DEFAULT_CAPITALIZE (false)
#define DEFAULT_PREFIX     ("OFFSET")

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
        FILE *outputf;
};

static bool should_export(tree decl, struct data *data)
{
        tree attr = lookup_attribute(data->config.match_attribute, DECL_ATTRIBUTES(decl));
        return (attr != NULL_TREE);
}

static size_t get_field_offset(tree field)
{
        tree offset = DECL_FIELD_OFFSET(field);
        tree bitoffset = DECL_FIELD_BIT_OFFSET(field);
        gcc_assert(TREE_CODE(offset) == INTEGER_CST);
        gcc_assert(TREE_CODE(bitoffset) == INTEGER_CST);

        // TODO: Not sure about the length of HOST_WIDE_INT.
        size_t overall_offset = TREE_INT_CST_LOW(offset) + TREE_INT_CST_LOW(bitoffset);
        gcc_assert(TREE_INT_CST_LOW(offset) >= 0);
        gcc_assert(TREE_INT_CST_LOW(bitoffset) >= 0);

        STATIC_ASSERT(sizeof(TREE_INT_CST_LOW(offset)) >= sizeof((void *)0));

        return (overall_offset);
}

static void strncpycap(char *dest, const char *src, size_t len)
{
        for (int i = 0; i < len; i++) {
                dest[i] = TOUPPER(src[i]);
        }
}

static void write_offset(const char *struct_name, const char *field_name, size_t offset,
                         struct data *data)
{
        const char *stname = struct_name;
        const char *fname = field_name;

        if (data->config.capitalize) {
                const size_t slen = strlen(struct_name) + 1;
                char *s = (char *)xmalloc(slen * sizeof(*struct_name));
                strncpycap(s, struct_name, slen);

                const size_t flen = strlen(field_name) + 1;
                char *f = (char *)xmalloc(flen * sizeof(*field_name));
                strncpycap(f, field_name, flen);

                stname = s;
                fname = f;
        }

        gcc_assert(offset % 8 == 0);
        fprintf(data->outputf, "#define %s%s%s%s%s (%zu)\n", data->config.prefix,
                data->config.separator, stname, data->config.separator, fname, offset / 8);

        if (data->config.capitalize) {
                free((void *)stname);
                free((void *)fname);
        }
}

static void handle_struct_type(tree decl, const char *parent_name, size_t base_offset,
                               struct data *data)
{
        gcc_assert(TREE_CODE(decl) == RECORD_TYPE);

        tree struct_id = TYPE_IDENTIFIER(decl);
        // Anonymous structures don't have type-names.
        const char *struct_name = (struct_id != NULL) ? IDENTIFIER_POINTER(struct_id) : parent_name;

        for (tree field = TYPE_FIELDS(decl); field != NULL; field = TREE_CHAIN(field)) {
                if (!should_export(field, data)) {
                        continue;
                }

                const char *field_name = IDENTIFIER_POINTER(DECL_NAME(field));
                const size_t field_offset = base_offset + get_field_offset(field);

                write_offset(struct_name, field_name, field_offset, data);

                tree field_type = TREE_TYPE(field);
                bool is_structure = TREE_CODE(field_type) == RECORD_TYPE;
                // If it's not anonymous, we will handle it on another PLUGIN_FINISH_TYPE callback.
                bool field_struct_anon = TYPE_IDENTIFIER(field_type) == NULL;
                if (is_structure && field_struct_anon) {
                        const size_t prename_len = strlen(struct_name);
                        const size_t fieldname_len = strlen(field_name);
                        const size_t separator_len = strlen(data->config.separator);

                        char *struct_prefix =
                                (char *)xmalloc(prename_len + fieldname_len + separator_len);
                        strcpy(struct_prefix, struct_name);
                        strcpy(&struct_prefix[prename_len], data->config.separator);
                        strcpy(&struct_prefix[prename_len + separator_len], field_name);

                        handle_struct_type(field_type, struct_prefix, field_offset, data);

                        free(struct_prefix);
                }
        }
}

static void handle_attributes(void *gcc_data __unused, void *user_data)
{
        struct data *d = (struct data *)user_data;
        register_attribute(&d->attr);
}

static void handle_finish_type(void *gcc_data, void *user_data)
{
        tree type_decl = (tree)gcc_data;
        struct data *data = (struct data *)user_data;

        // Interested only in structs.
        if (TREE_CODE(type_decl) != RECORD_TYPE) {
                return;
        }

        // Ignore anonymous structs. They will be handled as parts of parent structures.
        // There is a problem with global anonymous structures though...
        // TODO: Handle global anonymous structures.
        bool anonymous_struct = TYPE_IDENTIFIER(type_decl) == NULL;
        if (anonymous_struct) {
                return;
        }

        // Prefix will be prepended only to anonymous structures.
        handle_struct_type(type_decl, NULL, 0, data);
}

static void handle_finish(void *gcc_data __unused, void *user_data)
{
        struct data *data = (struct data *)user_data;
        fclose(data->outputf);
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

                if (strcmp(arg.key, "attribute") == 0) {
                        c.match_attribute = arg.value;
                } else if (strcmp(arg.key, "output") == 0) {
                        c.output_file = arg.value;
                } else if (strcmp(arg.key, "separator") == 0) {
                        c.separator = arg.value;
                } else if (strcmp(arg.key, "capitalize") == 0) {
                        c.capitalize = true;
                } else if (strcmp(arg.key, "prefix") == 0) {
                        c.prefix = arg.value;
                } else {
                        fprintf(stderr, "Unknown argument: %s\n", arg.key);
                }
        }
        return (c);
}

int plugin_init(struct plugin_name_args *info, struct plugin_gcc_version *version __unused)
{
        // TODO: Version check
        static struct data data = { 0 };

        data.config = parse_args(info->argc, info->argv);
        if (data.config.output_file == NULL) {
                fprintf(stderr, "Provide location where to write output\n");
                return (EXIT_FAILURE);
        }

        data.outputf = fopen(data.config.output_file, "w+");
        if (data.outputf == NULL) {
                perror("Couldn't open output file");
                return (EXIT_FAILURE);
        }

        data.attr = (struct attribute_spec){
                data.config.match_attribute, 0, 0, false, false, false, false, NULL
        };

        register_callback(info->base_name, PLUGIN_FINISH, handle_finish, &data);
        register_callback(info->base_name, PLUGIN_FINISH_TYPE, handle_finish_type, &data);
        register_callback(info->base_name, PLUGIN_ATTRIBUTES, handle_attributes, &data);

        return (EXIT_SUCCESS);
}
