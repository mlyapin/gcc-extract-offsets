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
#define DEFAULT_MAX_LENGTH (256)

int plugin_is_GPL_compatible;

struct config {
        const char *match_attribute;
        const char *output_file;
        const char *separator;
        const char *prefix;
        size_t max_length;
        bool capitalize;
        bool append;
} CONFIG;

struct data {
        struct attribute_spec attr;
        struct list handled_records;
        char *name_buffer;
        FILE *outputf;
        struct {
                char *mem;
                size_t current;
                size_t max;
        } buffer;
} DATA;

static char *strncpycap(char *dest, const char *src, size_t len)
{
        for (int i = 0; i < len; i++) {
                dest[i] = TOUPPER(src[i]);
        }
        return (dest);
}

///
/// Appends the separator and a name to the buffer.
///
/// Returns previous "current" position.
///
static size_t buffer_append(const char *str)
{
        gcc_assert(str != NULL);

        size_t previous_pos = DATA.buffer.current;

        typedef char *(*cpyf)(char *dest, const char *src, size_t len);
        cpyf f = CONFIG.capitalize ? strncpycap : strncpy;

// -1 for '\0'.
#define safeappend(STR) \
        f(&DATA.buffer.mem[DATA.buffer.current], (STR), DATA.buffer.max - DATA.buffer.current - 1)

        // No need to add the separrator at the beginning.
        if (DATA.buffer.current > 0) {
                safeappend(CONFIG.separator);
                DATA.buffer.current += strlen(CONFIG.separator);
        }

        safeappend(str);
        DATA.buffer.current += strlen(str);
#undef safeappend

        if (DATA.buffer.current >= DATA.buffer.max - 1) {
                DATA.buffer.mem[DATA.buffer.max - 1] = '\0';
                fprintf(stderr,
                        "Oops. The names of your structures are too long (or you have too many nested structures).\n"
                        "Please increase the buffer with the \"max_length\" argument.\n"
                        "For example, try to add -fplugin-arg-extract_offsets-max_length=%zu to the GCC invocation.\n"
                        "Right now, the buffer contains: \"%s\"\n",
                        DATA.buffer.max << 1, DATA.buffer.mem);
                exit(EXIT_FAILURE);
        }
        gcc_assert(DATA.buffer.current == strlen(DATA.buffer.mem));

        return (previous_pos);
}

static void buffer_reset_to(size_t pos)
{
        gcc_assert(pos <= DATA.buffer.max);

        // Not an error, but we don't know what is stored there.
        gcc_assert(pos <= DATA.buffer.current);

        DATA.buffer.current = pos;
        DATA.buffer.mem[pos] = '\0';
}

static bool should_export(tree decl)
{
        tree attr = lookup_attribute(CONFIG.match_attribute, DECL_ATTRIBUTES(decl));
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

static const char *get_strname(tree tnode)
{
        gcc_assert(DECL_P(tnode) || TYPE_P(tnode));
        tree t;
        if (DECL_P(tnode)) {
                t = DECL_NAME(tnode);
        } else {
                t = TYPE_IDENTIFIER(tnode);
        }

        return (t != NULL ? IDENTIFIER_POINTER(t) : NULL);
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

static void save_offset(size_t offset)
{
        gcc_assert(offset % 8 == 0);
        fprintf(DATA.outputf, "%s%s %zu\n", CONFIG.prefix, DATA.buffer.mem, offset / 8);
}

static void process_construct(tree construct, size_t base_offset)
{
        gcc_assert(struct_or_union(construct));

        if (list_contains(&DATA.handled_records, construct)) {
                return;
        }

        size_t precons_pos = 0;
        bool named_cons = false;
        {
                const char *consname = get_strname(construct);
                if (consname != NULL) {
                        precons_pos = buffer_append(get_strname(construct));
                        named_cons = true;
                }
        }

        for (tree field = TYPE_FIELDS(construct); field != NULL; field = TREE_CHAIN(field)) {
                const size_t field_offset = base_offset + get_field_bitoffset(field);

                size_t prefield_pos = 0;
                bool named_field = false;
                {
                        const char *field_namestr = get_strname(field);
                        if (field_namestr != NULL) {
                                prefield_pos = buffer_append(field_namestr);
                                named_field = true;
                        }
                }

                if (should_export(field)) {
                        gcc_assert(named_field);
                        save_offset(field_offset);
                }

                // If it's not anonymous, we will handle it on another PLUGIN_FINISH_TYPE callback.
                tree field_type = TREE_TYPE(field);
                if (struct_or_union(field_type) && is_anonymous(field_type)) {
                        process_construct(field_type, field_offset);
                }

                if (named_field) {
                        buffer_reset_to(prefield_pos);
                }
        }

        if (named_cons) {
                buffer_reset_to(precons_pos);
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

        process_construct(type, 0);
}

static void handle_attributes(void *gcc_data __unused, void *user_data __unused)
{
        register_attribute(&DATA.attr);
}

static void handle_finish(void *gcc_data __unused, void *user_data __unused)
{
        list_destroy(&DATA.handled_records);
        fclose(DATA.outputf);
        free(DATA.buffer.mem);
}

static struct config parse_args(int argc, struct plugin_argument *argv)
{
        struct config c = {
                .match_attribute = DEFAULT_ATTRIBUTE,
                .output_file = DEFAULT_OUTPUT,
                .separator = DEFAULT_SEPARATOR,
                .prefix = DEFAULT_PREFIX,
                .max_length = DEFAULT_MAX_LENGTH,
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
                } else if (argument_is("max_length")) {
                        size_t s = atoi(arg.value != NULL ? arg.value : "");
                        if (s > 0) {
                                c.max_length = s;
                        } else {
                                fprintf(stderr, "Wrong buffer_size, using default value %zu\n",
                                        c.max_length);
                        }
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
        CONFIG = parse_args(info->argc, info->argv);
        gcc_assert(CONFIG.output_file);

        const char *fmode = CONFIG.append ? "a" : "w";
        DATA.outputf = fopen(CONFIG.output_file, fmode);
        if (DATA.outputf == NULL) {
                perror("Couldn't open output file");
                return (EXIT_FAILURE);
        }

        DATA.attr = (struct attribute_spec){
                CONFIG.match_attribute, 0, 0, false, false, false, false, NULL
        };

        list_init(&DATA.handled_records);

        DATA.buffer.max = CONFIG.max_length;
        DATA.buffer.mem = (char *)xmalloc(DATA.buffer.max);
        gcc_assert(DATA.buffer.mem);
        DATA.buffer.current = 0;

        register_callback(info->base_name, PLUGIN_FINISH_TYPE, process_type, NULL);
        register_callback(info->base_name, PLUGIN_FINISH, handle_finish, NULL);
        register_callback(info->base_name, PLUGIN_ATTRIBUTES, handle_attributes, NULL);

        return (EXIT_SUCCESS);
}
