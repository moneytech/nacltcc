/// @file tinycc.cc
/// This example demonstrates loading, running and scripting a very simple NaCl
/// module.  To load the NaCl module, the browser first looks for the
/// CreateModule() factory method (at the end of this file).  It calls
/// CreateModule() once to load the module code from your .nexe.  After the
/// .nexe code is loaded, CreateModule() is not called again.
///
/// Once the .nexe code is loaded, the browser than calls the CreateInstance()
/// method on the object returned by CreateModule().  It calls CreateInstance()
/// each time it encounters an <embed> tag that references your NaCl module.
///
/// The browser can talk to your NaCl module via the postMessage() Javascript
/// function.  When you call postMessage() on your NaCl module from the browser,
/// this becomes a call to the HandleMessage() method of your pp::Instance
/// subclass.  You can send messages back to the browser by calling the
/// PostMessage() method on your pp::Instance.  Note that these two methods
/// (postMessage() in Javascript and PostMessage() in C++) are asynchronous.
/// This means they return immediately - there is no waiting for the message
/// to be handled.  This has implications in your program design, particularly
/// when mutating property values that are exposed to both the browser and the
/// NaCl module.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include <ppapi/cpp/instance.h>
#include <ppapi/cpp/module.h>
#include <ppapi/cpp/var.h>

#include <nacl-mounts/base/MainThreadRunner.h>
#include <nacl-mounts/base/UrlLoaderJob.h>
#include <nacl-mounts/memory/MemMount.h>

#include "../libtcc.h"

using namespace std;

extern "C" int mount(const char *type, const char *dir, int flags, void *data);
extern "C" int umount(const char *path);
extern "C" int simple_tar_extract(const char *path);

static void* InitFileSystemThread(void* data);

/// The Instance class.  One of these exists for each instance of your NaCl
/// module on the web page.  The browser will ask the Module object to create
/// a new Instance for each occurence of the <embed> tag that has these
/// attributes:
///     type="application/x-nacl"
///     src="tinycc.nmf"
/// To communicate with the browser, you must override HandleMessage() for
/// receiving messages from the borwser, and use PostMessage() to send messages
/// back to the browser.  Note that this interface is entirely asynchronous.
class TinyccInstance : public pp::Instance {
 public:
  /// The constructor creates the plugin-side instance.
  /// @param[in] instance the handle to the browser-side plugin instance.
  explicit TinyccInstance(PP_Instance instance)
    : pp::Instance(instance),
      id_(0) {
    PostMessage(pp::Var("status:INITIALIZED"));
  }

  virtual bool Init(uint32_t argc, const char* argn[], const char* argv[]) {
    runner_.reset(new MainThreadRunner(this));
    pthread_t th;
    pthread_create(&th, NULL, &InitFileSystemThread, this);
    return true;
  }

  virtual ~TinyccInstance() {
  }

  /// Handler for messages coming in from the browser via postMessage().  The
  /// @a var_message can contain anything: a JSON string; a string that encodes
  /// method names and arguments; etc.  For example, you could use
  /// JSON.stringify in the browser to create a message that contains a method
  /// name and some parameters, something like this:
  ///   var json_message = JSON.stringify({ "myMethod" : "3.14159" });
  ///   nacl_module.postMessage(json_message);
  /// On receipt of this message in @a var_message, you could parse the JSON to
  /// retrieve the method name, match it to a function call, and then call it
  /// with the parameter.
  /// @param[in] var_message The message posted by the browser.
  virtual void HandleMessage(const pp::Var& var_message) {
    if (!var_message.is_string()) {
      PostMessage(pp::Var("unexpected type: " + var_message.DebugString()));
      return;
    }

    errors_.clear();
    const string& msg = var_message.AsString();
    size_t colon_offset = msg.find(':');
    if (colon_offset == string::npos) {
      PostMessage(pp::Var("invalid command: " + var_message.DebugString()));
    }

    const string& cmd = msg.substr(0, colon_offset);
    int output_type = TCC_OUTPUT_MEMORY;
    if (cmd == "preprocess") {
      output_type = TCC_OUTPUT_PREPROCESS;
    } else if (cmd == "compile") {
      output_type = TCC_OUTPUT_OBJ;
    } else if (cmd == "run") {
      output_type = TCC_OUTPUT_MEMORY;
    } else {
      PostMessage(pp::Var("invalid command: " + cmd));
      return;
    }

    string code, stdin_contents;
    ParseStringArgs(msg.c_str() + colon_offset + 1, &code, &stdin_contents);

    close(0);
    close(1);
    close(2);
    int fd;
    char stdin_filename[256];
    sprintf(stdin_filename, "/tmp/stdin%d.txt", id_);
    fd = open(stdin_filename, O_WRONLY | O_CREAT | O_TRUNC);
    write(fd, stdin_contents.data(), stdin_contents.size());
    close(fd);
    fd = open(stdin_filename, O_RDONLY);
    assert(fd == 0);
    char stdout_filename[256];
    sprintf(stdout_filename, "/tmp/stdout%d.txt", id_);
    fd = open(stdout_filename, O_WRONLY | O_CREAT | O_TRUNC);
    assert(fd == 1);
    char stderr_filename[256];
    sprintf(stderr_filename, "/tmp/stderr%d.txt", id_);
    fd = open(stderr_filename, O_WRONLY | O_CREAT | O_TRUNC);
    assert(fd == 2);

    char input_filename[256];
    sprintf(input_filename, "/tmp/input%d.c", id_++);
    fd = open(input_filename, O_WRONLY | O_CREAT | O_TRUNC);
    assert(fd >= 0);
    write(fd, code.data(), code.size());
    close(fd);

    ScopedTCCState tcc_state;
    TCCState* s1 = tcc_state.get();
    tcc_set_error_func(s1, this, &TinyccInstance::HandleTCCError);
    tcc_set_output_type(s1, output_type);
    tcc_add_include_path(s1, "/data/usr/include");
    tcc_add_include_path(s1, "/data/usr/lib/tcc/include");
    FILE* fp = NULL;
    if (output_type == TCC_OUTPUT_PREPROCESS) {
      fp = fopen("/tmp/out", "wb");
      tcc_set_outfile(s1, fp);
    }
    tcc_add_file(s1, input_filename);
    if (output_type == TCC_OUTPUT_OBJ) {
      tcc_output_file(s1, "/tmp/out");
    }

    int status = -1;
    if (errors_.empty() && output_type == TCC_OUTPUT_MEMORY) {
      char* argv[] = {
        (char*)"./a.out", NULL
      };
      status = tcc_run(s1, 1, argv);
    }

    string out;

    unlink(input_filename);

    close(0);
    close(1);
    close(2);

    if (errors_.empty()) {
      fd = open(stdout_filename, O_RDONLY);
      if (fd >= 0) {
        string o;
        ReadFromFD(fd, &o);
        if (!o.empty()) {
          out += "=== STDOUT ===\n";
          out += o;
        }
      }
      close(fd);
      unlink(stdout_filename);
      fd = open(stderr_filename, O_RDONLY);
      if (fd >= 0) {
        string o;
        ReadFromFD(fd, &o);
        if (!o.empty()) {
          out += "=== STDERR ===\n";
          out += o;
        }
      }
      close(fd);
      unlink(stderr_filename);
      if (output_type == TCC_OUTPUT_MEMORY) {
        out += "=== EXIT STATUS ===\n";
        char buf[256];
        sprintf(buf, "%d\n", status);
        out += buf;
      }
    } else {
      out += "=== COMPILE ERROR ===\n";
      for (size_t i = 0; i < errors_.size(); i++) {
          out += errors_[i];
          out += '\n';
      }
    }

    if (output_type == TCC_OUTPUT_OBJ ||
        output_type == TCC_OUTPUT_PREPROCESS) {
      if (!out.empty()) {
        out += "\n=== OUTPUT ===\n";
      }

      if (output_type == TCC_OUTPUT_PREPROCESS) {
        fclose(fp);
      }
      int fd = open("/tmp/out", O_RDONLY);
      if (fd < 0) {
        PostMessage(pp::Var(
                      string("failed to read output: ") + strerror(errno)));
        return;
      }
      string o;
      ReadFromFD(fd, &o);

      if (output_type == TCC_OUTPUT_PREPROCESS) {
        PostMessageChecked(out + o);
      } else {
        string hex;
        char buf[20];
        for (int i = 0; i < (int)o.size(); i += 16) {
          sprintf(buf, "%07x:", i);
          hex += buf;
          for (int j = 0; j < 16; j++) {
            if (j % 2 == 0)
              hex += ' ';
            if (i + j < (int)o.size()) {
              sprintf(buf, "%02x", (unsigned char)o[i+j]);
              hex += buf;
            }
          }
          hex += "  ";
          for (int j = 0; j < 16 && i + j < (int)o.size(); j++) {
            char c = o[i+j];
            if (isprint(c))
              hex += c;
            else
              hex += '.';
          }
          hex += "\n";
        }
        PostMessageChecked(out + hex);
      }
      return;
    }

    PostMessageChecked(out);
  }

  static void HandleTCCError(void* data, const char* msg) {
    TinyccInstance* self = (TinyccInstance*)data;
    self->errors_.push_back(msg);
  }

  void InitFileSystem() {
    PostMessageFromThread("status:DOWNLOADING DATA");
    UrlLoaderJob *job = new UrlLoaderJob;
    job->set_url("data.tar");
    std::vector<char> data;
    job->set_dst(&data);
    runner_->RunJob(job);

    PostMessageFromThread("status:WRITING DATA");
    int fd = open("/data.tar", O_CREAT | O_WRONLY);
    if (fd < 0) {
      PostMessageFromThread("status:WRITE DATA FAILED");
      return;
    }
    write(fd, &data[0], data.size());
    close(fd);

    mkdir("/tmp", 0777);
    mkdir("/data", 0777);
    chdir("/data");
    PostMessageFromThread("status:EXTRACTING DATA");
    int r = simple_tar_extract("/data.tar");
    if (r != 0) {
      PostMessageFromThread("status:EXTRACT DATA FAILED");
    }

#ifdef __x86_64__
    PostMessageFromThread("status:READY (64bit)");
#else
    PostMessageFromThread("status:READY (32bit)");
#endif
  }

private:
  class PostMessageJob : public MainThreadJob {
  public:
    PostMessageJob(const string& msg) : msg_(msg) {}
    virtual ~PostMessageJob() {}
    virtual void Run(MainThreadRunner::JobEntry* entry) {
      pp::Instance* self = MainThreadRunner::ExtractPepperInstance(entry);
      self->PostMessage(pp::Var(msg_));
      MainThreadRunner::ResultCompletion(entry, 0);
    }
  private:
    string msg_;
  };

  struct ScopedTCCState {
    ScopedTCCState() {
      s1_ = tcc_new();
    }
    ~ScopedTCCState() {
      tcc_delete(s1_);
    }
    TCCState* get() {
      return s1_;
    }
  private:
    TCCState* s1_;
  };

  void PostMessageChecked(const string& msg) {
    if (msg.empty())
      PostMessage("=== NO OUTPUT ===");
    else
      PostMessage(pp::Var(msg));
  }

  void PostMessageFromThread(const string& msg) {
    PostMessageJob* job = new PostMessageJob(msg);
    runner_->RunJob(job);
  }

  static void ReadFromFD(int fd, string* out) {
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    char* buf = (char*)malloc(size + 1);
    buf[size] = '\0';
    read(fd, buf, size);
    out->assign(buf, buf+size);
    close(fd);
  }

  static string ReadFromFile(const char* file) {
    int fd = open(file, O_RDONLY);
    string s;
    ReadFromFD(fd, &s);
    return s;
  }

  void ParseStringArgs(const char* p, string* code, string* stdin_contents) {
    p = ParseStringArg(p, code);
    p = ParseStringArg(p, stdin_contents);
  }

  const char* ParseStringArg(const char* p, string* s) {
    if (!p || !*p)
      return NULL;
    int len = 0;
    while (isdigit(*p)) {
      len *= 10;
      len += *p - '0';
      p++;
    }
    if (!*p || *p != ' ')
      return NULL;
    p++;
    s->assign(p, p + len);
    return p + len;
  }

  auto_ptr<MainThreadRunner> runner_;
  auto_ptr<MemMount> mount_;
  vector<string> errors_;
  int id_;
};

void* InitFileSystemThread(void* data) {
  TinyccInstance* self = (TinyccInstance*)data;
  self->InitFileSystem();
  return NULL;
}

/// The Module class.  The browser calls the CreateInstance() method to create
/// an instance of your NaCl module on the web page.  The browser creates a new
/// instance for each <embed> tag with type="application/x-nacl".
class TinyccModule : public pp::Module {
 public:
  TinyccModule() : pp::Module() {}
  virtual ~TinyccModule() {}

  /// Create and return a TinyccInstance object.
  /// @param[in] instance The browser-side instance.
  /// @return the plugin-side instance.
  virtual pp::Instance* CreateInstance(PP_Instance instance) {
    return new TinyccInstance(instance);
  }
};

namespace pp {
/// Factory function called by the browser when the module is first loaded.
/// The browser keeps a singleton of this module.  It calls the
/// CreateInstance() method on the object you return to make instances.  There
/// is one instance per <embed> tag on the page.  This is the main binding
/// point for your NaCl module with the browser.
Module* CreateModule() {
  return new TinyccModule();
}
}  // namespace pp
