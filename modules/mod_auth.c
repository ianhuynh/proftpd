/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 *  
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * Authentication module for ProFTPD
 * $Id: mod_auth.c,v 1.2 1999-02-12 19:37:58 flood Exp $
 */

#include "conf.h"

#include "privs.h"

/* From the core module */
extern int core_display_file(const char *,const char *);
extern pid_t mpid;

module auth_module;

static int logged_in = 0;
static int auth_tries = 0;

/* check_auth is hooked into the main server's auth_hook function,
 * so that we can deny all commands until authentication is complete.
 */

int check_auth(cmd_rec *cmd)
{
  if(get_param_int(cmd->server->conf,"authenticated",FALSE) != 1) {
    send_response(R_530,"Please login with USER and PASS.");
    return FALSE;
  }

  return TRUE;
}

int _auth_shutdown(CALLBACK_FRAME)
{
  log_pri(LOG_ERR,"scheduled main_exit() never ran [from auth:_login_timeout], terminating.");
  end_login(1);
  return 0;				/* Avoid compiler warning */
}

int _login_timeout(CALLBACK_FRAME)
{
  /* Is this the proper behavior when timing out? */
  send_response_async(R_421,"Login Timeout (%d seconds): closing control connection.",
                      TimeoutLogin);
  schedule(main_exit,0,(void*)LOG_NOTICE,"FTP login timed out, disconnected.",
           (void*)0,NULL);
  remove_timer(TIMER_IDLE,ANY_MODULE);
  remove_timer(TIMER_NOXFER,ANY_MODULE);
  add_timer(10,-1,&auth_module,_auth_shutdown);
  return 0;		/* Don't restart the timer */
}

int auth_init_child()
{
  /* Start the login timer */
  if(TimeoutLogin)
    add_timer(TimeoutLogin,TIMER_LOGIN,&auth_module,_login_timeout);
  return 0;
}

int auth_init()
{
  /* By default, enable auth checking */
  set_auth_check(check_auth);
  return 0;
}

static int _do_auth(pool *p, xaset_t *conf, char *u, char *pw)
{
  char *cpw = NULL;
  config_rec *c;

  if(conf) {
    c = find_config(conf,CONF_PARAM,"UserPassword",FALSE);

    while(c) {
      if(!strcmp(c->argv[0],u)) {
        cpw = (char*)c->argv[1];
        break;
      }

      c = find_config_next(c,c->next,CONF_PARAM,"UserPassword",FALSE);
    }
  }


  if(cpw) {
    if(!auth_getpwnam(p,u))
      return AUTH_NOPWD;
    return auth_check(p,cpw,u,pw);
  }

  return auth_authenticate(p,u,pw);
}

/* Handle group based authentication, only checked if pw
 * based fails
 */

static config_rec *_auth_group(pool *p, char *user, char **group,
                               char **ournamep, char **anonnamep, char *pass)
{
  config_rec *c;
  char *ourname = NULL,*anonname = NULL;
  char **grmem;
  struct group *grp;

  ourname = (char*)get_param_ptr(main_server->conf,"UserName",FALSE);
  if(ournamep && ourname)
    *ournamep = ourname;

  c = find_config(main_server->conf,CONF_PARAM,"GroupPassword",TRUE);

  if(c) do {
    grp = auth_getgrnam(p,c->argv[0]);

    if(!grp)
      continue;

    for(grmem = grp->gr_mem; *grmem; grmem++)
      if(!strcmp(*grmem,user)) {
        if(auth_check(p,c->argv[1],user,pass) == 0)
          break;
      }

    if(*grmem) {
      if(group)
        *group = c->argv[0];

      if(c->parent)
        c = c->parent;

      if(c->config_type == CONF_ANON)
        anonname = (char*)get_param_ptr(c->subset,"UserName",FALSE);
      if(anonnamep)
        *anonnamep = anonname;
      if(anonnamep && !anonname && ourname)
        *anonnamep = ourname;
      
      break;
    }
  } while((c = find_config_next(c,c->next,CONF_PARAM,"GroupPassword",TRUE)) != NULL);

  return c;
}

static void build_group_arrays(pool *p, struct passwd *xpw, char *name,
                            array_header **gids, array_header **groups)
{
  struct group *gr;
  struct passwd *pw = xpw;
  array_header *xgids,*xgroups;
  char **gr_mem;

  xgids = make_array(p,2,sizeof(int));
  xgroups = make_array(p,2,sizeof(char*));

  if(!pw && !name)
    return;

  if(!pw) {
    pw = auth_getpwnam(p,name);

    if(!pw)
      return;
  }

  if((gr = auth_getgrgid(p,pw->pw_gid)) != NULL)
    *((char**)push_array(xgroups)) =
                         pstrdup(p,gr->gr_name);

  auth_setgrent(p);

  while((gr = auth_getgrent(p)) != NULL && gr->gr_mem)
    for(gr_mem = gr->gr_mem; *gr_mem; gr_mem++) {
      if(!strcmp(*gr_mem,pw->pw_name)) {
        *((int*)push_array(xgids)) = (int)gr->gr_gid;
        if(pw->pw_gid != gr->gr_gid)
          *((char**)push_array(xgroups)) =
                         pstrdup(p,gr->gr_name);
        break;
      }
    }

  *gids = xgids;
  *groups = xgroups;
}

static int _init_groups(pool *p, gid_t addl_group)
{
  gid_t *gid_arr;
  int i,*session_gids;
  size_t ngids = session.gids->nelts;

  session_gids = session.gids->elts;
  gid_arr = palloc(p,sizeof(gid_t) * (ngids+1));

  for(i = 0; i < ngids; i++)
    gid_arr[i] = (gid_t)session_gids[i];
  
  gid_arr[i] = addl_group;

  return setgroups(ngids+1,gid_arr);
}

static config_rec *_auth_anonymous_group(pool *p, char *user)
{
  config_rec *c;
  int ret = 0;

  build_group_arrays(p,NULL,user,&session.gids,&session.groups);
  c = find_config(main_server->conf,CONF_PARAM,"AnonymousGroup",FALSE);

  if(c) do {
    ret = group_expression((char**)c->argv);
  } while(!ret && (c = find_config_next(c,c->next,CONF_PARAM,"AnonymousGroup",FALSE)) != NULL);
 
  return ret ? c : NULL;
}

static config_rec *_auth_resolve_user(pool *p,char **user,
                                      char **ournamep,
                                      char **anonnamep)
{
  config_rec *c,*topc;
  char *ourname,*anonname = NULL;
  int is_alias = 0, force_anon = 0;

  /* Precendence rules:
   *   1. Search for UserAlias directive.
   *   2. Search for Anonymous directive.
   *   3. Normal user login
   */

  ourname = (char*)get_param_ptr(main_server->conf,"UserName",FALSE);

  if(ournamep && ourname)
    *ournamep = ourname; 

  c = find_config(main_server->conf,CONF_PARAM,"UserAlias",TRUE);
  if(c) do {
    if(!strcmp(c->argv[0], "*") || !strcmp(c->argv[0],*user)) {
      is_alias = 1;
      break;
    }  
  } while((c = find_config_next(c,c->next,CONF_PARAM,"UserAlias",TRUE)) != NULL);

  /* if AuthAliasOnly is set, ignore this one and continue */
  topc = c;

  while(c && c->parent &&
             find_config(c->parent->set,CONF_PARAM,"AuthAliasOnly",FALSE)) {
    is_alias = 0;
    find_config_set_top(topc);
    c = find_config_next(c,c->next,CONF_PARAM,"UserAlias",TRUE);
    if(c && (!strcmp(c->argv[0],"*") || !strcmp(c->argv[0],*user)))
      is_alias = 1;
  }

  if(c) {
    *user = c->argv[1];

    /* If the alias is applied inside an <Anonymous> context, we have found
     * our anon block
     */

    if(c->parent && c->parent->config_type == CONF_ANON)
      c = c->parent;
    else
      c = NULL;
  }

  /* Next, search for an anonymous entry */

  if(!c)
    c = find_config(main_server->conf,CONF_ANON,NULL,FALSE);
  else
    find_config_set_top(c);

  if(c) do {
    anonname = (char*)get_param_ptr(c->subset,"UserName",FALSE);
    if(!anonname)
      anonname = ourname;

    if(anonname && !strcmp(anonname,*user)) {
       if(anonnamep)
         *anonnamep = anonname;
       break;
    }
  } while((c = find_config_next(c,c->next,CONF_ANON,NULL,FALSE)) != NULL);

  if(!c) {
    c = _auth_anonymous_group(p,*user);

    if(c)
      force_anon = 1;
  }

  if(!is_alias && !force_anon) {
    if(find_config((c ? c->subset :
                   main_server->conf),CONF_PARAM,"AuthAliasOnly",FALSE)) {
      
      if(c && c->config_type == CONF_ANON)
        c = NULL;
      else
        *user = NULL;

      if(*user && find_config(main_server->conf,CONF_PARAM,"AuthAliasOnly",FALSE))
        *user = NULL;

      if((!user || !c) && anonnamep)
        *anonnamep = NULL;
    }
  }

  return c;
}

static int _auth_check_ftpusers(xaset_t *s, const char *user)
{
  int res = 1;
  FILE *fp;
  char *u,buf[256];

  if(get_param_int(s,"UseFtpUsers",FALSE) != 0) {
    PRIVS_ROOT
    fp = fopen(FTPUSERS_PATH,"r");
    PRIVS_RELINQUISH

    if(!fp)
      return res;

    while(fgets(buf,sizeof(buf)-1,fp)) {
      buf[sizeof(buf)-1] = '\0'; CHOP(buf);

      u = buf; while(isspace((UCHAR)*u) && *u) u++;

      if(!*u || *u == '#')
        continue;

      if(!strcmp(u,user)) {
        res = 0;
        break;
      }
    }

    fclose(fp);
  }

  return res;
}

static int _auth_check_shell(xaset_t *s, const char *shell)
{
  int res = 1;
  FILE *shellf;
  char buf[256];

  if(get_param_int(s,"RequireValidShell",FALSE) != 0 &&
     (shellf = fopen(VALID_SHELL_PATH,"r")) != NULL) {
    res = 0;
    while(fgets(buf,sizeof(buf)-1,shellf)) {
      buf[sizeof(buf)-1] = '\0'; CHOP(buf);

      if(!strcmp(shell,buf)) {
        res = 1;
        break;
      }
    }

    fclose(shellf);
  }

  return res;
}

/* Determine any applicable chdirs
 */

static char *_get_default_chdir(pool *p, xaset_t *conf)
{
  config_rec *c;
  char *dir = NULL;
  int ret;

  c = find_config(conf,CONF_PARAM,"DefaultChdir",FALSE);

  while(c) {
    /* Check the groups acl */
    if(c->argc < 2) {
      dir = c->argv[0];
      break;
    }

    ret = group_expression(((char**)c->argv)+1);

    if(ret) {
      dir = c->argv[0];
      break;
    }

    c = find_config_next(c,c->next,CONF_PARAM,"DefaultChdir",FALSE);
  }

  /* if the directory is relative, concatenate w/ session.cwd
   */

  if(dir && *dir != '/' && *dir != '~')
    dir = pdircat(p,session.cwd,dir,NULL);

  return dir;
}

/* Determine if the user (non-anon) needs a default root dir
 * other than /
 */

static char *_get_default_root(pool *p)
{
  config_rec *c;
  char *dir = NULL;
  int ret;

  c = find_config(main_server->conf,CONF_PARAM,"DefaultRoot",FALSE);

  while(c) {
    /* Check the groups acl */
    if(c->argc < 2) {
      dir = c->argv[0];
      break;
    }

    ret = group_expression(((char**)c->argv)+1);

    if(ret) {
      dir = c->argv[0];
      break;
    }

    c = find_config_next(c,c->next,CONF_PARAM,"DefaultRoot",FALSE);
  }

  if(dir) {
    if(!strcmp(dir,"/"))
      dir = NULL;
    else {
      char *realdir;

      realdir = dir_realpath(p,dir);

      if(realdir)
        dir = realdir;
    }
  }

  return dir;
}

static struct passwd *passwd_dup(pool *p, struct passwd *pw)
{
  struct passwd *npw;
      
  npw = pcalloc(p,sizeof(struct passwd));
   
  npw->pw_name = pstrdup(p,pw->pw_name);
  npw->pw_passwd = pstrdup(p,pw->pw_passwd);
  npw->pw_uid = pw->pw_uid;
  npw->pw_gid = pw->pw_gid;
  npw->pw_gecos = pstrdup(p,pw->pw_gecos);
  npw->pw_dir = pstrdup(p,pw->pw_dir);
  npw->pw_shell = pstrdup(p,pw->pw_shell);
      
  return npw;
}

/* Next function (the biggie) handles all authentication, setting
 * up chroot() jail, etc.
 */

static int _setup_environment(pool *p, char *user, char *pass)
{
  struct passwd *pw;
  struct stat sbuf;
  config_rec *c;
  char *origuser,*ourname,*anonname = NULL,*anongroup = NULL,*ugroup = NULL;
  char ttyname[20];
  char *defroot = NULL,*defchdir = NULL,*xferlog = NULL;
  int aclp,i,force_anon = 0,wtmp_log = -1,showsymlinks;

  /********************* Authenticate the user here *********************/

  session.hide_password = TRUE;

  origuser = user;
  c = _auth_resolve_user(p,&user,&ourname,&anonname);

  if(!user) {
    log_pri(LOG_NOTICE,"failed login, '%s' is not a UserAlias.",origuser);
    goto auth_failure;
  }

  /* If c != NULL from this point on, we have an anonymous login */
  aclp = login_check_limits(main_server->conf,FALSE,TRUE,&i);

  if((pw = auth_getpwnam(p,user)) == NULL) {
    log_pri(LOG_NOTICE,"failed login, can't find user '%s'",user);
    goto auth_failure;
  }

  /* security: other functions perform pw lookups, thus we need to make
   * a local copy of the user just looked up
   */

  pw = passwd_dup(p,pw);

  if(pw->pw_uid == 0) {
    /* If RootLogin is set to true, we allow this... even though we
     * still log a warning. :)
     */

    if(get_param_int((c ? c->subset : main_server->conf),"RootLogin",FALSE) != 1) {
      log_auth(LOG_CRIT,"SECURITY VIOLATION: root login attempted from %s [%s] to %s:%i",
	       session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
      return 0;
    } else
      log_auth(LOG_WARNING,"root login from %s [%s] to %s:%i",
	       session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
  }

  session.user = pstrdup(p,pw->pw_name);
  session.group = pstrdup(p,auth_gid_name(p,pw->pw_gid));

  /* set force_anon (for AnonymousGroup) and build a custom
   * anonymous config for this session.
   */
  if(c && c->config_type != CONF_ANON) {
    force_anon = 1;

    defroot = _get_default_root(session.pool);
    if(!defroot)
      defroot = pstrdup(session.pool,pw->pw_dir);

    c = (config_rec*)pcalloc(session.pool,sizeof(config_rec));
    c->config_type = CONF_ANON;
    c->name = defroot;

    anonname = pw->pw_name;

    /* hackery, we trick everything else by pointing the subset
     * at the main server's configuration.  tricky, eh?
     */
     c->subset = main_server->conf;
  }

  if(c) {
    if(!force_anon) {
        anongroup = (char*)get_param_ptr(c->subset,"GroupName",FALSE);
      if(!anongroup)
        anongroup = (char*)get_param_ptr(main_server->conf,"GroupName",FALSE);
    }

    if(!login_check_limits(c->subset,FALSE,TRUE,&i) || (!aclp && !i) ){
      log_auth(LOG_NOTICE,"ANON %s: LIMIT access denies login from %s [%s] to %s:%i",
                          origuser,
                          session.c->remote_name,
                          inet_ascii(p,session.c->remote_ipaddr),
			  inet_ascii(p,session.c->local_ipaddr),
			  session.c->local_port);
      goto auth_failure;
    }
  }

  if(!c && !aclp) {
    log_auth(LOG_NOTICE,"USER %s: LIMIT access denies login from %s [%s] to %s:%i",
             origuser,session.c->remote_name,
             inet_ascii(p,session.c->remote_ipaddr),
	     inet_ascii(p,session.c->local_ipaddr),
	     session.c->local_port);
    goto auth_failure;
  }

  if(!c || get_param_int(c->subset,"AnonRequirePassword",FALSE) == 1) {
    int authcode;

    if(c)
      authcode = _do_auth(p,c->subset,user,pass);
    else
      authcode = _do_auth(p,main_server->conf,user,pass);

    if(authcode) {
      /* Normal authentication has failed, see if group authentication
       * passes
       */

      if((c = _auth_group(p,user,&anongroup,&ourname,&anonname,pass)) != NULL) {
        if(c->config_type != CONF_ANON) {
          c = NULL;
          ugroup = anongroup; anongroup = NULL;
        }

        authcode = 0;
      }
    }
      
    bzero(pass,strlen(pass));

    switch(authcode) {
    case AUTH_NOPWD:
      log_auth(LOG_NOTICE,"USER %s: no such user found from %s [%s] to %s:%i",
               user,session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
      break;
    case AUTH_BADPWD:
      log_auth(LOG_NOTICE,"USER %s: incorrect password from %s [%s] to %s:%i",
               origuser,session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
      break;
    case AUTH_AGEPWD:
      log_auth(LOG_NOTICE,"USER %s: password expired from %s [%s] to %s:%i",
	       user,session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
      break;
    case AUTH_DISABLEDPWD:
      log_auth(LOG_NOTICE,"USER %s: account disabled from %s [%s] to %s:%i",
	       user,session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
      break;
    };

    if(authcode != 0)
      goto auth_failure;
  } else if(c)
    session.hide_password = FALSE;

/* Flood - 7/10/97, not sure what setutent() was used for, but it
 * certainly looks unnecessary now.
 */

 /* setutent(); */

  auth_setgrent(p);

  if(!_auth_check_shell((c ? c->subset : main_server->conf),pw->pw_shell)) {
    log_auth(LOG_NOTICE,"failed login '%s' from %s [%s] to %s:%i (invalid shell)",
               user,session.c->remote_name,
	       inet_ascii(p,session.c->remote_ipaddr),
	       inet_ascii(p,session.c->local_ipaddr),
	       session.c->local_port);
    goto auth_failure;
  }

  if(!_auth_check_ftpusers((c ? c->subset : main_server->conf),pw->pw_name)) {
    log_auth(LOG_NOTICE,"failed login '%s' from %s [%s] to %s:%i (user in %s)",
             pw->pw_name,session.c->remote_name,
             inet_ascii(p,session.c->remote_ipaddr),
	     inet_ascii(p,session.c->local_ipaddr),
	     session.c->local_port,
             FTPUSERS_PATH);
    goto auth_failure;
  }

  if(c) {
    struct group *grp;
    int add_userdir;
    char *u;
    
    u = (char *) get_param_int(main_server->conf,C_USER,FALSE);               
                                                                              
    add_userdir = get_param_int((c ? c->subset : main_server->conf),
				"UserDirRoot",FALSE);

    if(add_userdir && strcmp(u, user)) {
      session.anon_root = dir_realpath(session.pool, pdircat(session.pool,
							     c->name, u, NULL));
    } else {
      session.anon_root = dir_realpath(session.pool,c->name);
    }
    
    session.anon_user = pstrdup(session.pool,pass);

    if(!session.anon_root) {
      log_pri(LOG_ERR,"%s: Directory %s is not accessible.",
              session.user,c->name);
      add_response_err(R_530,"Unable to set anonymous privileges.");
      goto auth_failure;
    }
  
    strcpy(session.cwd,"/");
    xferlog = get_param_ptr(c->subset,"TransferLog",FALSE);

    if(anongroup) {
      grp = auth_getgrnam(p,anongroup);
      if(grp) {
        pw->pw_gid = grp->gr_gid;
        session.group = pstrdup(p,grp->gr_name);
      }
    }
  } else {
    struct group *grp;

    if(ugroup) {
      grp = auth_getgrnam(p,ugroup);
      if(grp) {
        pw->pw_gid = grp->gr_gid;
        session.group = pstrdup(p,grp->gr_name);
      }
    }

    strncpy(session.cwd,pw->pw_dir,MAX_PATH_LEN);
  }

  /* Get default chdir (if any) */
  defchdir = _get_default_chdir(p,(c ? c->subset : main_server->conf));
  
  if(defchdir)
    strncpy(session.cwd,defchdir,MAX_PATH_LEN);

  build_group_arrays(session.pool,pw,NULL,
                     &session.gids,&session.groups);


  /* check limits again to make sure deny/allow directives still permit
   * access.
   */

  if(!login_check_limits((c ? c->subset : main_server->conf),FALSE,TRUE,&i))
  {
    log_auth(LOG_AUTH,"%s: LIMIT access denies login (DenyGroup) from %s [%s] to %s:%i",
             origuser,session.c->remote_name,
             inet_ascii(p,session.c->remote_ipaddr),
	     inet_ascii(p,session.c->local_ipaddr),
	     session.c->local_port);
    goto auth_failure;
  }
  
  /* perform a dir fixup */
  resolve_defered_dirs(main_server);
  fixup_dirs(main_server,CF_DEFER);
  /* If running under an anonymous context, resolve all <Directory>
   * blocks inside it
   */
  if(c && c->subset)
    resolve_anonymous_dirs(c->subset);

  if(c)
    log_auth(LOG_NOTICE,"ANONYMOUS FTP login as '%s' from %s [%s] to %s:%i",
      origuser,session.c->remote_name,
      inet_ascii(p,session.c->remote_ipaddr),
      inet_ascii(p,session.c->local_ipaddr),
      session.c->local_port);
  else
    log_auth(LOG_NOTICE,"FTP login as '%s' from %s [%s] to %s:%i",
      origuser,session.c->remote_name,
      inet_ascii(p,session.c->remote_ipaddr),
      inet_ascii(p,session.c->local_ipaddr),
      session.c->local_port);

  /* Write the login to wtmp.  This must be done here because we won't
   * have access after we give up root.  This can result in falsified
   * wtmp entries if an error kicks the user out before we get
   * through with the login process.  Oh well.
   */

#if (defined(BSD) && (BSD >= 199103))
  sprintf(ttyname,"ftp%ld",(long)getpid());
#else
  sprintf(ttyname,"ftpd%d",(int)getpid());
#endif

  /* Perform wtmp logging only if not turned off in <Anonymous>
   * or the current server
   */

  if(c)
    wtmp_log = get_param_int(c->subset,"WtmpLog",FALSE);

  if(wtmp_log == -1)
    wtmp_log = get_param_int(main_server->conf,"WtmpLog",FALSE);

  PRIVS_ROOT

  if(wtmp_log != 0) {
    log_wtmp(ttyname,session.user,session.c->remote_name,
             session.c->remote_ipaddr);
    session.wtmp_log = TRUE;
  }

  /* Open the /var/run log for later writing */
  log_open_run(mpid,FALSE,TRUE);
  /* Open /var/log/ files */
  if(!xferlog) {
    if(c)
      xferlog = get_param_ptr(c->subset,"TransferLog",FALSE);
    if(!xferlog)
      xferlog = get_param_ptr(main_server->conf,"TransferLog",FALSE);
    if(!xferlog)
      xferlog = XFERLOG_PATH;
  }

  if(strcasecmp(xferlog,"NONE") == 0)
    log_open_xfer(NULL);
  else
    log_open_xfer(xferlog);

  _init_groups(p,pw->pw_gid);

  PRIVS_RELINQUISH

  /* Now check to see if the user has an applicable DefaultRoot */
  if(!c && (defroot = _get_default_root(session.pool))) {

    /* Make sure pass/group is open */
    auth_setpwent(p);
    auth_setgrent(p);

    /* On some unices the following is necessary to ensure the files
     * are open.  (BSDI 3.1)
     */
    auth_getpwent(p);
    auth_getgrent(p);

    PRIVS_ROOT

    if(chroot(defroot) == -1) {

      PRIVS_RELINQUISH

      add_response_err(R_530,"Unable to set default root directory.");
      log_pri(LOG_ERR,"%s chroot(\"%s\"): %s",session.user,
              defroot,strerror(errno));
      end_login(1);
    }

    PRIVS_RELINQUISH

    session.anon_root = defroot;

    /* Re-calc the new cwd based on this root dir.  If not applicable
     * place the user in / (of defroot)
     */

    if(strncmp(session.cwd,defroot,strlen(defroot)) == 0) {
      char *newcwd = &session.cwd[strlen(defroot)];

      if(*newcwd == '/')
        newcwd++;
      session.cwd[0] = '/';

      strcpy(&session.cwd[1],newcwd);
    } else
      strcpy(session.cwd,"/");
  }

  if(c) {
    /* Make sure pass/group is open */
    auth_setpwent(p);
    auth_setgrent(p);

    /* On some unices the following is necessary to ensure the files
     * are open.  (BSDI 3.1)
     */
    auth_getpwent(p);
    auth_getgrent(p);
  }

  PRIVS_ROOT

  if(c && chroot(session.anon_root) == -1) { 
    if(session.uid)
      _init_groups(p,session.gid);

    PRIVS_RELINQUISH

    add_response_err(R_530,"Unable to set anonymous privileges.");
    log_pri(LOG_ERR,"%s chroot(): %s",session.user,strerror(errno));
    
    end_login(1);
  }

  /* new in 1.1.x, I gave in and we don't give up root permanently..
   * sigh.
   */

  block_signals();

  PRIVS_ROOT

  setuid(0);
  setgid(0);

  PRIVS_SETUP(pw->pw_uid,pw->pw_gid)

  unblock_signals();

#ifdef HAVE_GETEUID
  if(getegid() != pw->pw_gid ||
     geteuid() != pw->pw_uid) {

    PRIVS_RELINQUISH

    add_response_err(R_530,"Unable to set user privileges.");
    log_pri(LOG_ERR,"%s setregid() or setreuid(): %s",
            session.user,strerror(errno));

    end_login(1);
  }
#endif

  /*
   *  session.uid = pw->pw_uid;
   */

  /* Overwrite original uid, so PRIVS_ macros no longer 
   * try to do anything 
   */

  /*
   * session.ouid = pw->pw_uid;
   * session.gid = pw->pw_gid;
   */

  /* chdir to the proper directory, do this even if anonymous
   * to make sure we aren't outside our chrooted space.
   */

  showsymlinks = get_param_int((c ? c->subset : main_server->conf),
                               "ShowSymlinks",FALSE);

  if(showsymlinks == -1)
    showsymlinks = 1;

  if(fs_chdir_canon(session.cwd,!showsymlinks) == -1) {
    add_response_err(R_530,"Unable to chdir.");
    log_pri(LOG_ERR,"%s chdir(\"%s\"): %s",session.user,
            session.cwd,strerror(errno));
    end_login(1);
  }

  strncpy(session.cwd,fs_getcwd(),sizeof(session.cwd));
  strncpy(session.vwd,fs_getvwd(),sizeof(session.vwd));


  /* check dynamic configuration */
  if(fs_stat("/",&sbuf) != -1)
    build_dyn_config(p,"/",&sbuf,1);

  if(c) {
    if(!session.hide_password)
      session.proc_prefix =
      pstrcat(permanent_pool,session.c->remote_name,
              ": anonymous/",pass,NULL);
    else
      session.proc_prefix =
      pstrcat(permanent_pool,session.c->remote_name,
              ": anonymous",NULL);

    session.anon_config = c;
    session.flags = SF_ANON;
  } else {
    session.proc_prefix = pstrdup(permanent_pool,session.c->remote_name);
            
    session.flags = 0;
  }

  /* Default transfer mode is ASCII */
  session.flags |= SF_ASCII;
 
  /* Authentication complete, user logged in, now kill the login
   * timer.
   */

  log_run_address(session.c->remote_name,session.c->remote_ipaddr);
  log_run_cwd(session.cwd);
  main_set_idle();

  remove_timer(TIMER_LOGIN,&auth_module);

  session.user = pstrdup(permanent_pool,session.user);
  session.group = pstrdup(permanent_pool,session.group);

  return 1;

auth_failure:
  session.user = session.group = NULL;
  session.gids = session.groups = NULL;
  session.wtmp_log = 0;
  return 0;
}
  
MODRET cmd_user(cmd_rec *cmd)
{
  int nopass = 0,cur = 0,hcur = 0;
  logrun_t *l;
  config_rec *c,*maxc;
  char *user,*origuser;
  int failnopwprompt = 0;
  int aclp,i;

  if(logged_in)
    return ERROR_MSG(cmd,R_503,"You are already logged in!");
  if(cmd->argc != 2)
    return ERROR_MSG(cmd,R_500,"'USER': command requires a parameter.");

  user = cmd->argv[1];

  remove_config(cmd->server->conf,C_USER,FALSE);
  add_config_param_set(&cmd->server->conf,C_USER,1,
                       pstrdup(cmd->server->pool,user));

  origuser = user;
  c = _auth_resolve_user(cmd->tmp_pool,&user,NULL,NULL);

  switch(get_param_int((c && c->config_type == CONF_ANON ? c->subset :
                     main_server->conf),"LoginPasswordPrompt",FALSE))
  {
    case 0: failnopwprompt = 1; break;
    default: failnopwprompt = 0; break;
  }

  if(failnopwprompt) {
    if(!user) {
      log_pri(LOG_NOTICE,"failed login, '%s' is not a UserAlias.",origuser);
      send_response(R_530,"Login incorrect.");
      end_login(0);
    }

    aclp = login_check_limits(main_server->conf,FALSE,TRUE,&i);

    if(c && c->config_type != CONF_ANON) {
      c = (config_rec*)pcalloc(session.pool,sizeof(config_rec));
      c->config_type = CONF_ANON;
      c->name = "";	/* don't really need this yet */
      c->subset = main_server->conf;
    }

    if(c) {
      if(!login_check_limits(c->subset,FALSE,TRUE,&i) || (!aclp && !i) ) {
              log_auth(LOG_NOTICE,"ANON %s: LIMIT access denies login from %s [%s] to %s:%i",
                          origuser,
                          session.c->remote_name,
                          inet_ascii(cmd->tmp_pool,session.c->remote_ipaddr),
			  inet_ascii(cmd->tmp_pool,session.c->local_ipaddr),
			  session.c->local_port);
              send_response(R_530,"Login incorrect.");
              end_login(0);
      }
    }

    if(!c && !aclp) {
      log_auth(LOG_NOTICE,"USER %s: LIMIT access denies login from %s [%s] to %s:%i",
               origuser,session.c->remote_name,
               inet_ascii(cmd->tmp_pool,session.c->remote_ipaddr),
	       inet_ascii(cmd->tmp_pool,session.c->local_ipaddr),
	       session.c->local_port);
      send_response(R_530,"Login incorrect.");
      end_login(0);
    }
  }
  
  /* Determine how many users are currently connected */

  if(user) {
    PRIVS_ROOT
    while((l = log_read_run(NULL)) != NULL)
      /* Make sure it matches our current server */
      if(l->server_ip.s_addr == main_server->ipaddr->s_addr &&
         l->server_port == main_server->ServerPort) {
        if((c && c->config_type == CONF_ANON && !strcmp(l->user,user)) || !c) {
          char *s, *d, ip[32];

          cur++;
          s = strchr (l->address, '[');
          d = ip;
          if (s != NULL) s++;
          while (*s && *s != ']') *d++ = *s++;
          *d = '\0';

          if(!strcmp(ip, inet_ntoa(*session.c->remote_ipaddr)))
            hcur++;
        }
      }
    PRIVS_RELINQUISH
  }

  remove_config(cmd->server->conf,"CURRENT-CLIENTS",FALSE);
  add_config_param_set(&cmd->server->conf,"CURRENT-CLIENTS",1,
                       (void*)cur);

  /* Try to determine what MaxClients applies to the user
   * (if any) and count through the runtime file to see
   * if this would exceed the max.
   */


  if(c && user && get_param_int(c->subset,"AnonRequirePassword",FALSE) != 1)
      nopass++;


  maxc = find_config((c ? c->subset : cmd->server->conf),
                  CONF_PARAM,"MaxClientsPerHost",FALSE);

  if(maxc) {
    int max = (int)maxc->argv[0];
    char *maxstr = "Sorry, maximum number clients (%m) from your host already connected.";
    char maxn[10];

    sprintf(maxn,"%d",max);
    if(maxc->argc > 1)
      maxstr = maxc->argv[1];
    
    if(hcur >= max) {
      send_response(R_530,"%s",
                    sreplace(cmd->tmp_pool,maxstr,"%m",maxn,NULL));

      log_auth(LOG_NOTICE,"connection refused (max clients per host %d)",
               max);
      end_login(0);
    }

  }


  maxc = find_config((c ? c->subset : cmd->server->conf),
                  CONF_PARAM,"MaxClients",FALSE);

  if(maxc) {
    int max = (int)maxc->argv[0];
    char *maxstr = "Sorry, maximum number of allowed clients (%m) already connected.";
    char maxn[10];

    sprintf(maxn,"%d",max);
    if(maxc->argc > 1)
      maxstr = maxc->argv[1];
    
    if(cur >= max) {
      send_response(R_530,"%s",
                    sreplace(cmd->tmp_pool,maxstr,"%m",maxn,NULL));

      log_auth(LOG_NOTICE,"connection refused (max clients %d)",
               max);
      end_login(0);
    }

  }

  /* Make sure the passwd file is open in case we chroot right away,
   * so we can still access it.
   */

  auth_setpwent(cmd->tmp_pool);
  auth_setgrent(cmd->tmp_pool);
  if(nopass)
    add_response(R_331,"Anonymous login ok, send your complete e-mail address as password.");
  else
    add_response(R_331,"Password required for %s.",cmd->argv[1]);

  session.gids = NULL;
  session.groups = NULL;
  session.user = NULL;
  session.group = NULL;

  return HANDLED(cmd);
}

MODRET cmd_pass(cmd_rec *cmd)
{
  char *display = NULL;
  char *user,*grantmsg;
  int res = 0;

#if 0
  if(cmd->argc < 2)
    return ERROR_MSG(cmd,R_500,"'PASS': command requires a parameter.");
#endif

  if(logged_in)
    return ERROR_MSG(cmd,R_503,"You are already logged in!");

  user = (char*)get_param_ptr(cmd->server->conf,C_USER,FALSE);

  if(!user)
    return ERROR_MSG(cmd,R_503,"Login with USER first.");
  
  if((res = _setup_environment(cmd->tmp_pool,user,cmd->arg)) == 1) {
    add_config_param_set(&cmd->server->conf,"authenticated",1,(void*)1);
    set_auth_check(NULL);

    if(session.flags & SF_ANON)
      display = (char*)get_param_ptr(session.anon_config->subset,
                                     "DisplayLogin",FALSE);
    if(!display)
      display = (char*)get_param_ptr(cmd->server->conf,
                                     "DisplayLogin",FALSE);

    if(display)
      core_display_file(R_230,display);

    if((grantmsg = 
        (char*)get_param_ptr((session.anon_config ? session.anon_config->subset :
                              cmd->server->conf),"AccessGrantMsg",FALSE)) != NULL) {
      grantmsg = sreplace(cmd->tmp_pool,grantmsg,"%u",user,NULL);

      add_response(R_230,"%s",grantmsg,NULL);
    } else {
      if(session.flags & SF_ANON)
        add_response(R_230,"Anonymous access granted, restrictions apply.");
      else
        add_response(R_230,"User %s logged in.",user);
    }

    logged_in = 1;
    return HANDLED(cmd);
  }

  remove_config(cmd->server->conf,C_USER,FALSE);

  if(res == 0) {
    int max;

    max = get_param_int(main_server->conf,"MaxLoginAttempts",FALSE);
    if(max == -1)
      max = 3;

    if(++auth_tries >= max) {
      send_response(R_530,"Login incorrect");
      log_auth(LOG_NOTICE,"Maximum login attempts exceeded from %s [%s] to %s:%i",
               session.c->remote_name,
               inet_ascii(cmd->tmp_pool,session.c->remote_ipaddr),
	       inet_ascii(cmd->tmp_pool,session.c->local_ipaddr),
	       session.c->local_port);
      end_login(0);
    }

    return ERROR_MSG(cmd,R_530,"Login incorrect.");
  }

  return HANDLED(cmd);
}

MODRET set_rootlogin(cmd_rec *cmd)
{
  CHECK_ARGS(cmd,1);
  CHECK_CONF(cmd,CONF_ROOT|CONF_VIRTUAL|CONF_ANON|CONF_GLOBAL);

  add_config_param("RootLogin",1,(void*)get_boolean(cmd,1));
  return HANDLED(cmd);
}

MODRET set_loginpasswordprompt(cmd_rec *cmd)
{
  config_rec *c;

  CHECK_ARGS(cmd,1);
  CHECK_CONF(cmd,CONF_ROOT|CONF_VIRTUAL|CONF_ANON|CONF_GLOBAL);

  c = add_config_param("LoginPasswordPrompt",1,(void*)get_boolean(cmd,1));
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET add_defaultroot(cmd_rec *cmd)
{
  config_rec *c;
  char *dir,**argv;
  int argc;
  array_header *acl = NULL;

  CHECK_CONF(cmd,CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if(cmd->argc < 2)
    CONF_ERROR(cmd,"syntax: DefaultRoot <directory> [<group-expression>]");

  argv = cmd->argv;
  argc = cmd->argc-2;

  dir = *++argv;

  /* dir must be / or ~
   */

  if(*dir != '/' && *dir != '~')
    CONF_ERROR(cmd,pstrcat(cmd->tmp_pool,"(",dir,") absolute pathname "
              "required.",NULL));

  if(strchr(dir,'*'))
    CONF_ERROR(cmd,pstrcat(cmd->tmp_pool,"(",dir,") wildcards not allowed "
               "in pathname."));

  if(*(dir+strlen(dir)-1) != '/')
    dir = pstrcat(cmd->tmp_pool,dir,"/",NULL);

  acl = parse_group_expression(cmd->tmp_pool,&argc,argv);

  c = add_config_param("DefaultRoot",0);

  c->argc = argc+1;
  c->argv = pcalloc(c->pool,(argc+2) * sizeof(char*));
  argv = (char**)c->argv;
  *argv++ = pstrdup(c->pool,dir);

  if(argc && acl)
    while(argc--) {
      *argv++ = pstrdup(c->pool,*((char**)acl->elts));
      acl->elts = ((char**)acl->elts) + 1;
    }

  *argv = NULL;
  return HANDLED(cmd);
}

MODRET add_defaultchdir(cmd_rec *cmd)
{
  config_rec *c;
  char *dir,**argv;
  int argc;
  array_header *acl = NULL;

  CHECK_CONF(cmd,CONF_ROOT|CONF_VIRTUAL|CONF_ANON|CONF_GLOBAL);

  if(cmd->argc < 2)
    CONF_ERROR(cmd,"syntax: DefaultChdir <directory> [<group-expression>]");

  argv = cmd->argv;
  argc = cmd->argc-2;

  dir = *++argv;

  if(strchr(dir,'*'))
    CONF_ERROR(cmd,pstrcat(cmd->tmp_pool,"(",dir,") wildcards not allowed "
               "in pathname."));

  if(*(dir+strlen(dir)-1) != '/')
    dir = pstrcat(cmd->tmp_pool,dir,"/",NULL);

  acl = parse_group_expression(cmd->tmp_pool,&argc,argv);

  c = add_config_param("DefaultChdir",0);

  c->argc = argc+1;
  c->argv = pcalloc(c->pool,(argc+2) * sizeof(char*));
  argv = (char**)c->argv;
  *argv++ = pstrdup(c->pool,dir);

  if(argc && acl)
    while(argc--) {
      *argv++ = pstrdup(c->pool,*((char**)acl->elts));
      acl->elts = ((char**)acl->elts) + 1;
    }

  *argv = NULL;
  return HANDLED(cmd);
}

MODRET add_userdirroot (cmd_rec *cmd)
{
  CHECK_ARGS(cmd,1);
  CHECK_CONF (cmd, CONF_ANON);

  add_config_param("UserDirRoot",1,(void*)get_boolean(cmd,1));
  return HANDLED(cmd);
}

conftable auth_config[] = {
  { "RootLogin",		set_rootlogin,			NULL },
  { "LoginPasswordPrompt",	set_loginpasswordprompt,	NULL },
  { "DefaultRoot",		add_defaultroot,		NULL },
  { "DefaultChdir",		add_defaultchdir,		NULL },
  { "UserDirRoot",		add_userdirroot,		NULL },
  { NULL,			NULL,				NULL }
};

cmdtable auth_commands[] = {
  { CMD, C_USER, G_NONE, cmd_user,	FALSE,	FALSE, CL_AUTH },
  { CMD, C_PASS, G_NONE, cmd_pass,	FALSE,  FALSE, CL_AUTH },
  { 0, NULL }
};

/* Module interface */

module auth_module = {
  NULL,NULL,				/* Always NULL */
  0x20,					/* API Version 2.0 */
  "auth",
  auth_config,	
  auth_commands,
  NULL,
  auth_init,auth_init_child
};

