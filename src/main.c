#include "duv.h"
#include "misc.h"
#include "dhttp_parser.h"

static uv_loop_t loop;

// Sync readfile using libuv APIs as an API function.
static duk_ret_t duv_loadfile(duk_context *ctx) {
  const char* path = duk_require_string(ctx, 0);
  uv_fs_t req;
  int fd = 0;
  uint64_t size;
  char* chunk;
  uv_buf_t buf;

  if (uv_fs_open(&loop, &req, path, O_RDONLY, 0644, NULL) < 0) goto fail;
  fd = req.result;
  if (uv_fs_fstat(&loop, &req, fd, NULL) < 0) goto fail;
  size = req.statbuf.st_size;
  chunk = duk_alloc(ctx, size);
  buf = uv_buf_init(chunk, size);
  if (uv_fs_read(&loop, &req, fd, &buf, 1, 0, NULL) < 0) goto fail;
  duk_push_lstring(ctx, chunk, size);
  duk_free(ctx, chunk);
  uv_fs_close(&loop, &req, fd, NULL);
  uv_fs_req_cleanup(&req);

  return 1;

  fail:
  if (fd) uv_fs_close(&loop, &req, fd, NULL);
  uv_fs_req_cleanup(&req);
  duk_error(ctx, DUK_ERR_ERROR, "%s: %s: %s", uv_err_name(req.result), uv_strerror(req.result), path);
}

struct duv_list {
  const char* part;
  int offset;
  int length;
  struct duv_list* next;
};
typedef struct duv_list duv_list_t;

static duv_list_t* duv_list_node(const char* part, int start, int end, duv_list_t* next) {
  duv_list_t *node = malloc(sizeof(*node));
  node->part = part;
  node->offset = start;
  node->length = end - start;
  node->next = next;
  return node;
}

static duk_ret_t duv_path_join(duk_context *ctx) {
  duv_list_t *list = NULL;
  int absolute = 0;

  // Walk through all the args and split into a linked list
  // of segments
  {
    int i;
    for (i = 0; i < duk_get_top(ctx); ++i) {
      const char* part = duk_require_string(ctx, i);
      int j;
      int start = 0;
      int length = strlen(part);
      if (!i && part[0] == 0x2f) {
        absolute = 1;
      }
      while (start < length && part[start] == 0x2f) { ++start; }
      for (j = start; j < length; ++j) {
        if (part[j] == 0x2f) {
          if (start < j) {
            list = duv_list_node(part, start, j, list);
            start = j;
            while (start < length && part[start] == 0x2f) { ++start; }
          }
        }
      }
      if (start < j) {
        list = duv_list_node(part, start, j, list);
      }
    }
  }

  // Run through the list in reverse evaluating "." and ".." segments.
  {
    int skip = 0;
    duv_list_t *prev = NULL;
    while (list) {
      duv_list_t *node = list;

      // Ignore segments with "."
      if (node->length == 1 &&
          node->part[node->offset] == 0x2e) {
        goto skip;
      }

      // Ignore segments with ".." and grow the skip count
      if (node->length == 2 &&
          node->part[node->offset] == 0x2e &&
          node->part[node->offset + 1] == 0x2e) {
        ++skip;
        goto skip;
      }

      // Consume the skip count
      if (skip > 0) {
        --skip;
        goto skip;
      }

      list = node->next;
      node->next = prev;
      prev = node;
      continue;

      skip:
        list = node->next;
        free(node);
    }
    list = prev;
  }

  // Merge the list into a single `/` delimited string.
  // Free the remaining list nodes.
  {
    int count = 0;
    if (absolute) {
      duk_push_string(ctx, "/");
      ++count;
    }
    while (list) {
      duv_list_t *node = list;
      duk_push_lstring(ctx, node->part + node->offset, node->length);
      ++count;
      if (node->next) {
        duk_push_string(ctx, "/");
        ++count;
      }
      list = node->next;
      free(node);
    }
    duk_concat(ctx, count);
  }
  return 1;
}

static duk_ret_t duv_require(duk_context *ctx) {
  int is_main = 0;

  dschema_check(ctx, (const duv_schema_entry[]) {
    {"id", duk_is_string},
    {NULL}
  });

  // push Duktape
  duk_get_global_string(ctx, "Duktape");

  // id = Duktape.modResolve(this, id);
  duk_get_prop_string(ctx, -1, "modResolve");
  duk_push_this(ctx);
  {
    // Check if we're in main
    duk_get_prop_string(ctx, -1, "exports");
    if (duk_is_undefined(ctx, -1)) { is_main = 1; }
    duk_pop(ctx);
  }
  duk_dup(ctx, 0);
  duk_call(ctx, 2);
  duk_replace(ctx, 0);

  // push Duktape.modLoaded
  duk_get_prop_string(ctx, -1, "modLoaded");

  // push Duktape.modLoaded[id];
  duk_dup(ctx, 0);
  duk_get_prop(ctx, -2);

  // if (typeof Duktape.modLoaded[id] === 'object') {
  //   return Duktape.modLoaded[id].exports;
  // }
  if (duk_is_object(ctx, -1)) {
    duk_get_prop_string(ctx, -1, "exports");
    return 1;
  }

  // pop Duktape.modLoaded[id]
  duk_pop(ctx);

  // push module = { id: id, exports: {} }
  duk_push_object(ctx);
  if (is_main) {
    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "main");
  }
  else {
    duk_push_this(ctx);
    duk_put_prop_string(ctx, -2, "parent");
  }
  duk_dup(ctx, 0);
  duk_put_prop_string(ctx, -2, "id");
  duk_push_object(ctx);
  duk_put_prop_string(ctx, -2, "exports");

  // Duktape.modLoaded[id] = module
  duk_dup(ctx, 0);
  duk_dup(ctx, -2);
  duk_put_prop(ctx, -4);

  // remove Duktape.modLoaded
  duk_remove(ctx, -2);

  // push Duktape.modLoad(module)
  duk_get_prop_string(ctx, -2, "modLoad");
  duk_dup(ctx, -2);
  duk_call(ctx, 1);

  // if ret !== undefined module.exports = ret;
  if (duk_is_undefined(ctx, -1)) {
    duk_pop(ctx);
  }
  else {
    duk_put_prop_string(ctx, -2, "exports");
  }

  duk_get_prop_string(ctx, -1, "exports");

  return 1;
}

// Default implementation for modResolve
// Duktape.modResolve = function (parent, id) {
//   return pathJoin(parent.id, "..", id);
// };
static duk_ret_t duv_mod_resolve(duk_context *ctx) {
  dschema_check(ctx, (const duv_schema_entry[]) {
    {"parent", duk_is_object},
    {"id", duk_is_string},
    {NULL}
  });

  duk_push_c_function(ctx, duv_path_join, DUK_VARARGS);
  duk_get_prop_string(ctx, 0, "id");
  duk_push_string(ctx, "..");
  duk_dup(ctx, 1);
  duk_call(ctx, 3);

  return 1;
}

// Default Duktape.modLoad implementation
// return Duktape.modCompile(module, loadFile(module.id));
static duk_ret_t duv_mod_load(duk_context *ctx) {
  dschema_check(ctx, (const duv_schema_entry[]) {
    {"module", duk_is_object},
    {NULL}
  });

  duk_get_global_string(ctx, "Duktape");
  duk_get_prop_string(ctx, -1, "modCompile");
  duk_dup(ctx, 0);
  duk_push_c_function(ctx, duv_loadfile, 1);
  duk_get_prop_string(ctx, -2, "id");
  duk_call(ctx, 1);
  duk_call(ctx, 2);

  return 1;
}


// Given a module and js code, compile the code and execute as CJS module
// return the result of the compiled code ran as a function.
static duk_ret_t duv_mod_compile(duk_context *ctx) {
  // Check the args
  dschema_check(ctx, (const duv_schema_entry[]) {
    {"module", duk_is_object},
    {"code", dschema_is_data},
    {NULL}
  });
  duk_to_string(ctx, 1);

  // Wrap the code
  duk_push_string(ctx, "function(require,module){require=require.bind(module);var exports=module.exports;");
  duk_dup(ctx, 1);
  duk_push_string(ctx, "}");
  duk_concat(ctx, 3);

  // Compile to a function
  duk_get_prop_string(ctx, 0, "id");
  duk_compile(ctx, DUK_COMPILE_FUNCTION);
  duk_push_c_function(ctx, duv_require, 1);
  duk_dup(ctx, 0);
  duk_call(ctx, 2);

  return 1;
}

static duk_ret_t duv_main(duk_context *ctx) {
  const char* path = duk_require_string(ctx, 0);

  duk_push_global_object(ctx);
  duk_dup(ctx, -1);
  duk_put_prop_string(ctx, -2, "global");

  // Load duv module into global uv
  duk_push_c_function(ctx, dukopen_uv, 0);
  duk_call(ctx, 0);
  duk_put_prop_string(ctx, -2, "uv");

  // Load dhttp_parser module into global http_parser
  duk_push_c_function(ctx, dukopen_http_parser, 0);
  duk_call(ctx, 0);
  duk_put_prop_string(ctx, -2, "http_parser");

  // Replace the module loader with Duktape 2.x polyfill.
  duk_get_prop_string(ctx, -1, "Duktape");
  duk_del_prop_string(ctx, -1, "modSearch");
  duk_push_c_function(ctx, duv_mod_compile, 2);
  duk_put_prop_string(ctx, -2, "modCompile");
  duk_push_c_function(ctx, duv_mod_resolve, 2);
  duk_put_prop_string(ctx, -2, "modResolve");
  duk_push_c_function(ctx, duv_mod_load, 1);
  duk_put_prop_string(ctx, -2, "modLoad");
  duk_pop(ctx);

  // Put in some quick globals to test things.
  duk_push_c_function(ctx, duv_path_join, DUK_VARARGS);
  duk_put_prop_string(ctx, -2, "pathJoin");

  duk_push_c_function(ctx, duv_loadfile, 1);
  duk_put_prop_string(ctx, -2, "loadFile");

  // require.call({id:uv.cwd()+"/main.c"}, path);
  duk_push_c_function(ctx, duv_require, 1);
  duk_push_object(ctx);
  duk_push_c_function(ctx, duv_cwd, 0);
  duk_call(ctx, 0);
  duk_push_string(ctx, "/main.c");
  duk_concat(ctx, 2);
  duk_put_prop_string(ctx, -2, "id");
  duk_push_string(ctx, path);
  duk_call_method(ctx, 1);

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}

int main(int argc, char *argv[]) {
  duk_context *ctx = NULL;
  uv_loop_init(&loop);

  uv_setup_args(argc, argv);

  if (argc < 2) {
    fprintf(stderr, "Usage: dukluv script.js\n");
    exit(1);
  }

  // Tie loop and context together
  ctx = duk_create_heap(NULL, NULL, NULL, &loop, NULL);
  if (!ctx) {
    fprintf(stderr, "Problem initiailizing duktape heap\n");
    return -1;
  }
  loop.data = ctx;

  duk_push_c_function(ctx, duv_main, 1);
  duk_push_string(ctx, argv[1]);
  if (duk_pcall(ctx, 1)) {
    fprintf(stderr, "\nUncaught Exception:\n");
    if (duk_is_object(ctx, -1)) {
      duk_get_prop_string(ctx, -1, "stack");
      fprintf(stderr, "\n%s\n\n", duk_get_string(ctx, -1));
      duk_pop(ctx);
    }
    else {
      fprintf(stderr, "\nThrown Value: %s\n\n", duk_json_encode(ctx, -1));
    }
    duk_destroy_heap(ctx);
    return 1;
  }

  duk_destroy_heap(ctx);
  return 0;
}
