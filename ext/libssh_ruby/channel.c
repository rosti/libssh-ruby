#include "libssh_ruby.h"
#include <ruby/thread.h>

#define RAISE_IF_ERROR(rc) \
  if ((rc) == SSH_ERROR)   \
  libssh_ruby_raise(ssh_channel_get_session(holder->channel))

VALUE rb_cLibSSHChannel;

static ID id_stderr, id_timeout;

static void channel_mark(void *);
static void channel_free(void *);
static size_t channel_memsize(const void *);

struct ChannelHolderStruct {
  ssh_channel channel;
  VALUE session;
};
typedef struct ChannelHolderStruct ChannelHolder;

static const rb_data_type_t channel_type = {
    "ssh_channel",
    {channel_mark, channel_free, channel_memsize, {NULL, NULL}},
    NULL,
    NULL,
    RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE channel_alloc(VALUE klass) {
  ChannelHolder *holder = ALLOC(ChannelHolder);
  holder->channel = NULL;
  holder->session = Qundef;
  return TypedData_Wrap_Struct(klass, &channel_type, holder);
}

static void channel_mark(void *arg) {
  ChannelHolder *holder = arg;
  if (holder->channel != NULL) {
    rb_gc_mark(holder->session);
  }
}

static void channel_free(void *arg) {
  ChannelHolder *holder = arg;

  if (holder->channel != NULL) {
    /* XXX: ssh_channel is freed by ssh_session */
    /* ssh_channel_free(holder->channel); */
    holder->channel = NULL;
  }

  ruby_xfree(holder);
}

static size_t channel_memsize(RB_UNUSED_VAR(const void *arg)) {
  return sizeof(ChannelHolder);
}

static VALUE m_initialize(VALUE self, VALUE session) {
  ChannelHolder *holder;
  SessionHolder *session_holder;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  session_holder = libssh_ruby_session_holder(session);
  holder->channel = ssh_channel_new(session_holder->session);
  holder->session = session;

  return self;
}

static VALUE m_close(VALUE self) {
  ChannelHolder *holder;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  RAISE_IF_ERROR(ssh_channel_close(holder->channel));

  return Qnil;
}

static VALUE m_open_session(VALUE self) {
  ChannelHolder *holder;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  RAISE_IF_ERROR(ssh_channel_open_session(holder->channel));

  if (rb_block_given_p()) {
    return rb_ensure(rb_yield, Qnil, m_close, self);
  } else {
    return Qnil;
  }
}

static VALUE m_request_exec(VALUE self, VALUE cmd) {
  ChannelHolder *holder;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  RAISE_IF_ERROR(
      ssh_channel_request_exec(holder->channel, StringValueCStr(cmd)));

  return Qnil;
}

struct nogvl_read_args {
  ssh_channel channel;
  char *buf;
  uint32_t count;
  int is_stderr;
  int timeout;
  int rc;
};

static void *nogvl_read(void *ptr) {
  struct nogvl_read_args *args = ptr;
  args->rc = ssh_channel_read_timeout(args->channel, args->buf, args->count,
                                      args->is_stderr, args->timeout);
  return NULL;
}

static VALUE m_read(int argc, VALUE *argv, VALUE self) {
  ChannelHolder *holder;
  VALUE count, opts;
  const ID table[] = {id_stderr, id_timeout};
  VALUE kwvals[sizeof(table) / sizeof(*table)];
  struct nogvl_read_args args;
  VALUE ret;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  rb_scan_args(argc, argv, "10:", &count, &opts);
  Check_Type(count, T_FIXNUM);
  rb_get_kwargs(opts, table, 0, 2, kwvals);
  if (kwvals[0] == Qundef) {
    args.is_stderr = 0;
  } else {
    args.is_stderr = RTEST(kwvals[0]) ? 1 : 0;
  }
  if (kwvals[1] == Qundef) {
    args.timeout = -1;
  } else {
    Check_Type(kwvals[1], T_FIXNUM);
    args.timeout = FIX2INT(kwvals[1]);
  }
  args.channel = holder->channel;
  args.count = FIX2UINT(count);
  args.buf = ALLOC_N(char, args.count);
  rb_thread_call_without_gvl(nogvl_read, &args, RUBY_UBF_IO, NULL);

  ret = rb_utf8_str_new(args.buf, args.rc);
  ruby_xfree(args.buf);
  return ret;
}

static VALUE m_eof_p(VALUE self) {
  ChannelHolder *holder;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  return ssh_channel_is_eof(holder->channel) ? Qtrue : Qfalse;
}

struct nogvl_poll_args {
  ssh_channel channel;
  int timeout;
  int is_stderr;
  int rc;
};

static void *nogvl_poll(void *ptr) {
  struct nogvl_poll_args *args = ptr;
  args->rc =
      ssh_channel_poll_timeout(args->channel, args->timeout, args->is_stderr);
  return NULL;
}

static VALUE m_poll(int argc, VALUE *argv, VALUE self) {
  ChannelHolder *holder;
  VALUE opts;
  const ID table[] = {id_stderr, id_timeout};
  VALUE kwvals[sizeof(table) / sizeof(*table)];
  struct nogvl_poll_args args;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  rb_scan_args(argc, argv, "00:", &opts);
  rb_get_kwargs(opts, table, 0, 2, kwvals);
  if (kwvals[0] == Qundef) {
    args.is_stderr = 0;
  } else {
    args.is_stderr = RTEST(kwvals[0]) ? 1 : 0;
  }
  if (kwvals[1] == Qundef) {
    args.timeout = -1;
  } else {
    Check_Type(kwvals[1], T_FIXNUM);
    args.timeout = FIX2INT(kwvals[1]);
  }

  args.channel = holder->channel;
  rb_thread_call_without_gvl(nogvl_poll, &args, RUBY_UBF_IO, NULL);
  RAISE_IF_ERROR(args.rc);

  if (args.rc == SSH_EOF) {
    return Qnil;
  } else {
    return INT2FIX(args.rc);
  }
}

static VALUE m_get_exit_status(VALUE self) {
  ChannelHolder *holder;
  int rc;

  TypedData_Get_Struct(self, ChannelHolder, &channel_type, holder);
  rc = ssh_channel_get_exit_status(holder->channel);
  if (rc == -1) {
    return Qnil;
  } else {
    return INT2FIX(rc);
  }
}

void Init_libssh_channel(void) {
  rb_cLibSSHChannel = rb_define_class_under(rb_mLibSSH, "Channel", rb_cObject);
  rb_define_alloc_func(rb_cLibSSHChannel, channel_alloc);

  rb_define_method(rb_cLibSSHChannel, "initialize",
                   RUBY_METHOD_FUNC(m_initialize), 1);
  rb_define_method(rb_cLibSSHChannel, "open_session",
                   RUBY_METHOD_FUNC(m_open_session), 0);
  rb_define_method(rb_cLibSSHChannel, "close", RUBY_METHOD_FUNC(m_close), 0);
  rb_define_method(rb_cLibSSHChannel, "request_exec",
                   RUBY_METHOD_FUNC(m_request_exec), 1);
  rb_define_method(rb_cLibSSHChannel, "read", RUBY_METHOD_FUNC(m_read), -1);
  rb_define_method(rb_cLibSSHChannel, "poll", RUBY_METHOD_FUNC(m_poll), -1);
  rb_define_method(rb_cLibSSHChannel, "eof?", RUBY_METHOD_FUNC(m_eof_p), 0);
  rb_define_method(rb_cLibSSHChannel, "get_exit_status",
                   RUBY_METHOD_FUNC(m_get_exit_status), 0);

  id_stderr = rb_intern("stderr");
  id_timeout = rb_intern("timeout");
}
