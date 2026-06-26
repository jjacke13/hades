#include "hades/tool/subprocess.h"
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace hades {
static double mono(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }

ProcResult run_subprocess(const std::vector<std::string>& argv, const std::string& in,
                          double timeout_s, std::size_t mem_mb){
  ::signal(SIGPIPE, SIG_IGN);
  if(argv.empty()) return {-1,"","",false};
  int ip[2], op[2], ep[2];
  if(pipe(ip)!=0) return {-1,"","",false};
  if(pipe(op)!=0){ close(ip[0]); close(ip[1]); return {-1,"","",false}; }
  if(pipe(ep)!=0){ close(ip[0]); close(ip[1]); close(op[0]); close(op[1]); return {-1,"","",false}; }
  pid_t pid=fork();
  if(pid<0){ for(int fd: {ip[0],ip[1],op[0],op[1],ep[0],ep[1]}) close(fd); return {-1,"","",false}; }
  if(pid==0){                                    // child
    dup2(ip[0],0); dup2(op[1],1); dup2(ep[1],2);
    for(int fd: {ip[0],ip[1],op[0],op[1],ep[0],ep[1]}) close(fd);
    if(mem_mb>0){ rlimit rl{ mem_mb*1024*1024, mem_mb*1024*1024 }; setrlimit(RLIMIT_AS,&rl); }
    std::vector<char*> a; a.reserve(argv.size()+1);
    for(const auto& s: argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  // parent
  close(ip[0]); close(op[1]); close(ep[1]);
  fcntl(ip[1], F_SETFL, O_NONBLOCK);
  ProcResult r{0,"","",false};
  std::size_t in_off=0;
  if(in.empty()){ close(ip[1]); ip[1]=-1; }
  bool out_open=true, err_open=true;
  const double deadline=mono()+timeout_s;
  while(out_open || err_open){
    int ms=(int)((deadline-mono())*1000);
    if(ms<=0){ kill(pid,SIGKILL); r.timed_out=true; break; }
    pollfd fds[3]; int n=0, i_out=-1, i_err=-1, i_in=-1;
    if(out_open){ fds[n]={op[0],POLLIN,0}; i_out=n++; }
    if(err_open){ fds[n]={ep[0],POLLIN,0}; i_err=n++; }
    if(ip[1]>=0){ fds[n]={ip[1],POLLOUT,0}; i_in=n++; }
    int pr=poll(fds,n,ms);
    if(pr<0){ if(errno==EINTR) continue; break; }
    if(pr==0){ kill(pid,SIGKILL); r.timed_out=true; break; }
    char buf[4096];
    if(i_out>=0 && (fds[i_out].revents & (POLLIN|POLLHUP))){
      ssize_t k=read(op[0],buf,sizeof buf);
      if(k>0) r.out.append(buf,k);
      else if(k==0){ close(op[0]); out_open=false; }
      else if(errno!=EINTR && errno!=EAGAIN){ close(op[0]); out_open=false; }
    }
    if(i_err>=0 && (fds[i_err].revents & (POLLIN|POLLHUP))){
      ssize_t k=read(ep[0],buf,sizeof buf);
      if(k>0) r.err.append(buf,k);
      else if(k==0){ close(ep[0]); err_open=false; }
      else if(errno!=EINTR && errno!=EAGAIN){ close(ep[0]); err_open=false; }
    }
    if(i_in>=0 && (fds[i_in].revents & (POLLOUT|POLLERR|POLLHUP))){
      ssize_t k=write(ip[1], in.data()+in_off, in.size()-in_off);
      if(k>0){ in_off+=(std::size_t)k; if(in_off>=in.size()){ close(ip[1]); ip[1]=-1; } }
      else if(k<0 && errno!=EINTR && errno!=EAGAIN){ close(ip[1]); ip[1]=-1; } // EPIPE: child gone
    }
  }
  if(ip[1]>=0) close(ip[1]);
  if(out_open) close(op[0]);
  if(err_open) close(ep[0]);
  int st=0, rc;
  do { rc=waitpid(pid,&st,0); } while(rc<0 && errno==EINTR);
  if(!r.timed_out && WIFEXITED(st)) r.code=WEXITSTATUS(st);
  else r.code=-1;
  return r;
}
}  // namespace hades
