// Shell.

#include "types.h"
#include "user.h"
#include "fcntl.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

// Define constants for the up, down, left, and right arrow keys (SAME as in kbd.h)
#define KEY_UP 0xE2
#define KEY_DN 0xE3
#define KEY_LF 0xE4
#define KEY_RT 0xE5

#define HISTORY_SIZE 20 // Maximum number of previous commands to store
#define CMD_SIZE 100 // Maximum length for one specific command

static char history[HISTORY_SIZE][CMD_SIZE]; // Array of size 20x100 to store past commands
static int history_len = 0; // Number of commands stored so far

static char cwd[128] = "~/";

// Functions declarations 
static void redraw(const char *buf, int len, int cursor, int prev_len);
static int readline_xv6(char *buf, int nbuf);

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit();

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit();
    exec(ecmd->argv[0], ecmd->argv);
    printf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      printf(2, "open %s failed\n", rcmd->file);
      exit();
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait();
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait();
    wait();
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit();
}
#pragma GCC diagnostic pop

// This function will act as a replacement for the gets function which was previously used here
int getcmd(char *buf, int nbuf) {
  write(2, cwd, strlen(cwd));
  write(2, "$ ", 2); // Print the shell promopt
  memset(buf, 0, nbuf); // Clear the command buffer

  // Read the edited line (which can be handled with left/right arrows)
  if (readline_xv6(buf, nbuf) < 0) {
    return -1; // Returns -1 if error happened
  }
  return 0;
}

// This small helper function will write the specified char to the specified fd
static void putc_fd(int fd, char c) {
  write(fd, &c, 1);
}

// This helper function will fully redraw the command line on the screen. This is needed because natively there's no way to handle left/right arrow key editing.
static void redraw(const char *buf, int len, int cursor, int prev_len) {
  int i;
  putc_fd(2, '\r'); // Return to the start of the line
  write(2, cwd, strlen(cwd));
  write(2, "$ ", 2); // Reprint the prompt

  if (len > 0) {
    write(2, buf, len); // Print the current command
  }

  // If the newline is shorter than the old one, clear the chars that are leftover
  for (i = len; i < prev_len; i++) {
    putc_fd(2, ' ');
  }

  // Move the cursor back over empty/cleared spaces
  for (i = len; i < prev_len; i++) {
    putc_fd(2, '\b');
  }

  // Move the cursor left to its required position
  for (i = len; i > cursor; i--) {
    putc_fd(2, '\b');
  }
}

// This helper function will add a new command to the history buffer we have made
static void history_add(const char *line) {
  int len = strlen(line); // Length of the new command
  
  // Ignore empty commands
  if (len <= 0) {
    return;
  }

  // If the most recent command is a duplicate of the last one do not store it
  if (history_len > 0) {
    if (strcmp(history[history_len - 1], line) == 0) {
      return;
    }
  }

  // If the command buffer is not full, add the command and increment the number of commands
  if (history_len < HISTORY_SIZE) {
    strcpy(history[history_len], line);
    history_len++;
  } 
  
  // Else, the command buffer is full
  else {

    // Shift older commands to the left
    for (int i = 1; i < HISTORY_SIZE; i++) {
      strcpy(history[i - 1], history[i]);
    }

    strcpy(history[HISTORY_SIZE - 1], line); // Add the most recent command to the end
  }
}

// This helper function will support the cursor movement (and insertion/deletion of chars) and command history navigation
static int readline_xv6(char *buf, int nbuf) {
  int len = 0; // Chars in the buffer
  int cursor = 0; // Starting cursor position inside the buffer
  int prev_len = 0; // Previous printed length
  int hist_idx = history_len; // Start after the last command
  buf[0] = 0; // Start with an empty string

  while (1) {

    // Read the next input and save it into c (arrow keys are received as KEY_x)
    char c;
    int n = read(0, &c, 1);
    
    // If there is no more input we reached the end
    if (n < 1) {
      return -1;
    }

    // Enter key was pressed 
    if ((unsigned char)c == '\n' || (unsigned char)c == '\r') {
      buf[len] = 0; // End the buffer
      putc_fd(2, '\n'); // Move onto the next line
      history_add(buf); // Add the command to the command history
      return 0;
    }

    // Left arrow was pressed
    if ((unsigned char)c == KEY_LF) {
      
      // If the cursor is not all the way to the left, decrement it and redraw the command line to the user
      if (cursor > 0) {
        cursor--;
        redraw(buf, len, cursor, prev_len);
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }

    // Right arrow was pressed
    if ((unsigned char)c == KEY_RT) {

      // If the cursor is not all the way to the right, increment it and redraw the command line to the user
      if (cursor < len) {
        cursor++;
        redraw(buf, len, cursor, prev_len);
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }

    // Up arrow was pressed
    if ((unsigned char)c == KEY_UP) {

      // If we have command history to look through
      if (history_len > 0) {
        
        // Go backwards through the history once
        if (hist_idx > 0) {
          hist_idx--;
        }

        strcpy(buf, history[hist_idx]); // Get the history entry
        len = strlen(buf); // Update the length
        cursor = len; // Move the cursor to the end
        redraw(buf, len, cursor, prev_len); // Redraw the command line to the user
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }

    // Down arrow was pressed
    if ((unsigned char)c == KEY_DN) {

      // If we have command history to look through
      if (history_len > 0) {

        // Go forward through the command history once
        if (hist_idx < history_len) {
          hist_idx++;
        }

        //If the we are at the end, clear the buffer and set the length to 0
        if (hist_idx == history_len) {
          buf[0] = 0;
          len = 0;
        } 
        
        // Else, copy the command into the buffer, and gets its length
        else {
          strcpy(buf, history[hist_idx]);
          len = strlen(buf);
        }
        cursor = len; // Move the cursor to the end
        redraw(buf, len, cursor, prev_len); // Redraw the command line to the user
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }

    // If the backspace or delete key was pressed
    if ((unsigned char)c == 0x08 || (unsigned char)c == 0x7f) {

      // If there is a current command on the screen (cursor is pointing to something)
      if (cursor > 0) {

        // Shift the characters left
        for (int i = cursor - 1; i < len - 1; i++) {
          buf[i] = buf[i + 1];
        }
        len--; // Decrement the length as we deleted one char
        cursor--; // Move the cursor to the left
        buf[len] = 0; // Update the end of the string with a 0
        redraw(buf, len, cursor, prev_len); // Redraw the command line to the user
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }

    // This is for normal printable ASCII chars
    if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
      
      // Shift the chars to the right
      if (len < nbuf - 1) {
        for (int i = len; i > cursor; i--) {
          buf[i] = buf[i - 1];
        }
        buf[cursor] = (unsigned char)c; // Insert the new char at the cursor position
        cursor++; // Move the cursor to the right
        len++; // Increment the length
        buf[len] = 0; // Update the end of the string with a 0
        redraw(buf, len, cursor, prev_len); // Redraw the command line to the user
        prev_len = len; // Set the previous command length to the current command length
      }
      continue;
    }
  }
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
      // Chdir must be called by the parent, not the child.
      buf[strlen(buf)-1] = 0;  // chop \n
      char *path = buf + 3;
      if (chdir(path) < 0) {
        printf(2, "cannot cd %s\n", path);
      } else {
        if (path[0] == '/') {
          strcpy(cwd, path);
        } else {
          int len = strlen(cwd);
          if (strcmp(cwd, "/") != 0) {
            cwd[len] = '/';
            cwd[len + 1] = 0;
            len++;
          }
          int i = 0;
          while (path[i] != 0) {
            cwd[len++] = path[i++];
          }
          cwd[len] = 0;
        }
      }
      continue;
    }
    if (strcmp(buf, "pwd") == 0) {
      printf(1, "%s\n", cwd);
      continue;
    }
    if (strcmp(buf, "clear") == 0) {
      printf(1, "\033[2J\033[H");
      continue;
    }
    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait();
  }
  exit();
}

void
panic(char *s)
{
  printf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    printf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
