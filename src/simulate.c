#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#if !defined(SunOS_5)
#include <sys/dir.h>
#endif
#include <fcntl.h>
#include <setjmp.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <memory.h>
#if defined(sun)
#include <alloca.h>
#endif
#ifdef M_UNIX
#include <dirent.h>
#endif

#include "lint.h"
#include "lang.tab.h"
#include "interpret.h"
#include "object.h"
#include "sent.h"
#include "exec.h"
#include "comm.h"
#include "debug.h"

extern int errno;
extern int current_time;
extern int comp_flag;

char *inherit_file;

/* prevents infinite inherit loops.
   No, mark-and-sweep solution won't work.  Exercise for reader.  */
int num_objects_this_thread = 0; 

#ifndef NeXT
extern int readlink PROT((char *, char *, int));
extern int symlink PROT((char *, char *));
#endif /* NeXT */
     
#if !defined(hpux) && !defined(_AIX) && !defined(__386BSD__) \
	&& !defined(linux) && !defined(SunOS_5)
extern int fchmod PROT((int, int));
#endif /* !defined(hpux) && !defined(_AIX) */
char *last_verb;
     
extern int set_call PROT((struct object *, struct sentence *, int, int)),
       legal_path PROT((char *));
     
void pre_compile PROT((char *)),
       remove_interactive PROT((struct object *)),
       add_light PROT((struct object *, int)),
       add_action PROT((char *, char *, int)),
       add_verb PROT((char *, int)),
       ipc_remove(),
       set_snoop PROT((struct object *, struct object *)),
       print_lnode_status PROT((int)),
       start_new_file PROT((FILE *)), end_new_file(),
       move_or_destruct PROT((struct object *, struct object *)),
       load_ob_from_swap PROT((struct object *)), dump_malloc_data(),
       print_svalue PROT((struct svalue *)),
       debug_message_value(),
       destruct2();
     
extern int d_flag;
     
struct object *obj_list, *obj_list_destruct, *master_ob;
     
extern userid_t *backbone_uid;

struct object *current_object;      /* The object interpreting a function. */
struct object *command_giver;       /* Where the current command came from. */
struct object *current_interactive; /* The user who caused this execution */
     
int num_parse_error;		/* Number of errors in the parser. */
     
struct variable *find_status(str, must_find)
     char *str;
     int must_find;
{
  int i;
  
  for (i=0; (unsigned)i < current_object->prog->p.i.num_variables; i++) {
    if (strcmp(current_object->prog->p.i.variable_names[i].name, str) == 0)
      return &current_object->prog->p.i.variable_names[i];
  }
  if (!must_find)
    return 0;
  error("--Status %s not found in prog for %s\n", str,
	current_object->name);
  return 0;
}


/*
 * Give the correct uid and euid to a created object.
 */
int give_uid_to_object(ob)
     struct object *ob;
{
  struct svalue *ret;
  char *creator_name;
  struct object *tmp_ob;
  static char *creator_file_fname = (char *) 0;
  
  if (master_ob == 0)
    tmp_ob = ob;
  else 
    {
      assert_master_ob_loaded();
      tmp_ob = master_ob;
    }
  
  if (!current_object || !current_object->uid) {
    /*
     * Only for the master and void object. Note that
     * back_bone_uid is not defined when master.c is being loaded.
     */
    ob->uid = add_uid ("NONAME");
    ob->euid = NULL;
    
    return 1;
  }
  
  /*
   * Ask master.c who the creator of this object is.
   */
  push_string(ob->name, STRING_CONSTANT);
  if (!creator_file_fname)
    creator_file_fname = make_shared_string("creator_file");
  ret = apply(creator_file_fname, tmp_ob, 1);
  if (!ret)
    error("No function 'creator_file' in master.c!\n");
  if (ret->type != T_STRING) {
    struct svalue arg;
    /* This can be the case for objects in /ftp and /open. */
    arg.type = T_OBJECT;
    arg.u.ob = ob;
    destruct_object(&arg);
    error("Illegal object to load.\n");
  }
  creator_name = ret->u.string;
  /*
   * Now we are sure that we have a creator name.
   * Do not call apply() again, because creator_name will be lost !
   */
  if (strcmp(current_object->uid->name, creator_name) == 0) {
    /* 
     * The loaded object has the same uid as the loader.
     */
    ob->uid = current_object->uid;
    ob->euid = current_object->euid; /* FIXME - is this right? */
    return 1;
  }
  
#ifdef AUTO_TRUST_BACKBONE
  if (strcmp(backbone_uid->name, creator_name) == 0) {
    /*
     * The object is loaded from backbone. This is trusted, so we
     * let it inherit the value of eff_user.
     */
    ob->uid = current_object->euid;
    ob->euid = current_object->euid;
    return 1;
  }
#endif
  
  /*
   * The object is not loaded from backbone, nor from 
   * from the loading objects path. That should be an object
   * defined by another wizard. It can't be trusted, so we give it the
   * same uid as the creator. Also give it eff_user 0, which means that
   * user 'a' can't use objects from user 'b' to load new objects nor
   * modify files owned by user 'b'.
   *
   * If this effect is wanted, user 'b' must let his object do
   * 'seteuid()' to himself. That is the case for most rooms.
   */
  ob->uid = add_uid(creator_name);
#ifdef AUTO_SETEUID
  ob->euid = ob->uid;
#else
  ob->euid = NULL;
#endif
  return 1;
}


int init_object (ob)
     struct object *ob;
{
  init_stats_for_object (ob);
  add_objects (&ob->stats, 1);
  return give_uid_to_object (ob);
}



struct svalue *
  load_virtual_object(name)
char *name;
{
  static int loading_virtual_object = 0;
  struct svalue *v;
  
  push_string(name, STRING_MALLOC);
  loading_virtual_object++;
  v = apply_master_ob("compile_object", 1);
  loading_virtual_object--;
  if (!v || (v->type != T_OBJECT)) {
    fprintf(stderr, "Could not load descr for %s\n", name);
    error("Failed to load file: %s\n",name);
    return 0;
  }
  return v;
}

/*
 * Load an object definition from file. If the object wants to inherit
 * from an object that is not loaded, discard all, load the inherited object,
 * and reload again.
 *
 * In mudlib3.0 when loading inherited objects, their reset() is not called.
 * - why is this??  it makes no sense and causes a problem when a developer
 * inherits code from a real used item.  Say a room for example.  In this case
 * the room is loaded but is never set up properly, so when someone enters it
 * it's all messed up.  Realistically, I know that it's pretty bad style to
 * inherit from an object that's actually being used and isn't just a building
 * block, but I see no reason for this limitation.  It happens, and when it
 * does occur, produces mysterious results than can be hard to track down.
 * for now, I've reenabled resetting.  We'll see if anything breaks. -WF
 *
 * Save the command_giver, because reset() in the new object might change
 * it.
 * 
 */
struct object *load_object(lname, dont_reset)
     char *lname;
     int dont_reset;
{
  FILE *f;
  extern int total_lines;
  
  struct object *ob, *save_command_giver = command_giver;
  extern struct program *prog;
  extern char *current_file;
  struct stat c_st;
  int name_length;
  char real_name[200], name[200];
  char *p;
  
#ifdef VALID_CLONE
  if (current_object && master_ob) {
    char *orig;
    
    orig = lname;
    lname = check_valid_path(lname, current_object, "load_object", 0);
    if (!lname) {
      error("Insufficient permission to read file: %s\n", orig);
    }
  }
#endif
  
  if (++num_objects_this_thread > INHERIT_CHAIN_SIZE)
    error ("Inherit chain too deep: > %d\n",INHERIT_CHAIN_SIZE);
  if (current_object && current_object->euid == NULL)
    error("Can't load objects when no effective user.\n");
  /* don't allow consecutive "/"'s - Wayfarer */
  p = lname;
  while (*p)
    {
      if (*p == '/' && *(p + 1) == '/')
	{
	  error ("Filenames with consecutive /'s in them aren't allowed.\n");
	  return 0; 
	}
      p++;
    }
  /* Truncate possible .c in the object name. */
  /* Remove leading '/' if any. */
  while(lname[0] == '/')
    lname++;
  strncpy(name, lname, sizeof(name) - 1);
  name[sizeof name - 1] = '\0';
  name_length = strlen(name);
  if (name_length > sizeof name - 4)
    name_length = sizeof name - 4;
  name[name_length] = '\0';
  if (name[name_length-2] == '.' && name[name_length-1] == 'c') {
    name[name_length-2] = '\0';
    name_length -= 2;
  }
  /*
   * First check that the c-file exists.
   */
  (void) strcpy(real_name, name);
  (void) strcat(real_name, ".c");
  if (stat(real_name, &c_st) == -1) {
    struct svalue *v;
    
    if (!(v = load_virtual_object(name))) {
      return 0;
    }
    /* Now set the file name of the specified object correctly...*/
    ob = v->u.ob;
    remove_object_hash(ob);
    ob->name = string_copy(name);
    enter_object_hash(ob);
    ob->flags |= O_VIRTUAL;
    ob->load_time = current_time;
    return ob; 
  }
  /*
   * Check if it's a legal name.
   */
  if (!legal_path(real_name)) {
    fprintf(stderr, "Illegal pathname: %s\n", real_name);
    error("Illegal path name.\n");
    return 0;
  }
  if (comp_flag)
    fprintf(stderr, " compiling %s ...", real_name);
  f = fopen(real_name, "r");
  if (f == 0) {
    perror(real_name);
    error("Could not read the file.\n");
  }
  current_file = string_copy(real_name);	/* This one is freed below */
  start_new_file(f);
  compile_file();
  end_new_file();
  if (comp_flag)
    fprintf(stderr, " done\n");
  update_compile_av(total_lines);
  total_lines = 0;
  (void)fclose(f);
  FREE(current_file);
  current_file = 0;
  /* Sorry, can't handle objects without programs yet. */
  if (inherit_file == 0 && (num_parse_error > 0 || prog == 0)) {
    if (prog)
      free_prog(prog, 1);
    if (num_parse_error == 0 && prog == 0)
      error("No program in object!\n");
    error("Error in loading object\n");
  }
  /*
   * This is an iterative process. If this object wants to inherit an
   * unloaded object, then discard current object, load the object to be
   * inherited and reload the current object again. The global variable
   * "inherit_file" will be set by lang.y to point to a file name.
   */
  if (inherit_file) {
    char *tmp = inherit_file;
    if (prog) {
      free_prog(prog, 1);
      prog = 0;
    }
    if (strcmp(inherit_file, name) == 0) {
      FREE(inherit_file);
      inherit_file = 0;
      error("Illegal to inherit self.\n");
    }
    inherit_file = 0;
#if 0 /* MUDLIB3_NEED, It's very awkard to have to have a debug3 /JnA */
    load_object(tmp, 1);
#else
    load_object(tmp, 0);		/* Remove this feature for now */
#endif
    FREE(tmp);
    ob = load_object(name, dont_reset);
    ob->load_time = current_time;
    num_objects_this_thread--;
    return ob;
  }
  ob = get_empty_object(prog->p.i.num_variables);
  ob->name = string_copy(name);	/* Shared string is no good here */
  ob->prog = prog;
  ob->flags |= O_WILL_RESET; /* must be before reset is first called */
  ob->next_all = obj_list;
  obj_list = ob;
  enter_object_hash(ob);	/* add name to fast object lookup table */
  
  if (init_object(ob) && !dont_reset)
    reset_object(ob, 0);
  if (!(ob->flags & O_DESTRUCTED) &&
      function_exists("clean_up", ob)) {
    ob->flags |= O_WILL_CLEAN_UP;
  }
  
  command_giver = save_command_giver;
  if (d_flag > 1 && ob)
    debug_message("--%s loaded\n", ob->name);
  ob->load_time = current_time;
  num_objects_this_thread--;
  return ob;
}

char *make_new_name(str)
     char *str;
{
  static int i;
  char *p = DXALLOC(strlen(str) + 10, 97, "make_new_name");
  
  (void)sprintf(p, "%s#%d", str, i);
  i++;
  return p;
}


/*
 * Save the command_giver, because reset() in the new object might change
 * it.
 */
struct object *clone_object(str1)
     char *str1;
{
  struct object *ob, *new_ob;
  struct object *save_command_giver = command_giver;
  
  if(current_object && current_object->euid == 0){
    error("Object must call seteuid() prior to calling clone_object().\n");
  }
  
#ifdef VALID_CLONE
  if(current_object){
    char *orig;
    
    orig = str1;
    str1 = check_valid_path(str1, current_object, "clone_object", 0);
    if(!str1){
      error("Insufficient permission to read file: %s\n",orig);
    }
  }
#endif /* VALID_CLONE */
  
  num_objects_this_thread = 0;
  ob = find_object(str1);
  if (ob && !object_visible(ob)) ob = 0;
  /*
   * If the object self-destructed...
   */
  if(ob == 0) /* fix from 3.1.1 */
    return(0);
  if(ob == 0 || ob->super || (ob->flags & O_CLONE))
    if(!(ob->flags & O_VIRTUAL) || strrchr(str1,'#'))
      error("Cloning a bad object !\n");
    else {
      /* 
       * well... it's a virtual object.  So now we're going to "clone" it.
       */
      struct svalue *v;
      char *p;
      
      /* Remove leading '/' if any. */
      while(str1[0] == '/')
	str1++;
      p = str1;
      while(*p){
	if(*p == '/' && *(p + 1) == '/'){
	  error("Filenames with consecutive /'s in them aren't allowed.\n");
	  return(0); 
	}
	p++;
      }
      if (!(v = load_virtual_object(str1))) {
	return 0;
      }
      /* Now set the file name of the specified object correctly...*/
      new_ob = v->u.ob;
      remove_object_hash(new_ob);
      new_ob->name = make_new_name(str1);
      enter_object_hash(new_ob);
      new_ob->flags |= O_VIRTUAL;
      new_ob->load_time = current_time;
      command_giver = save_command_giver;
      return(new_ob);
      /* we can skip all of the stuff below since we were already cloned
	 once to have gotten to this stage.  */
    }
  /* We do not want the heart beat to be running for unused copied objects */
  if(ob->flags & O_HEART_BEAT) 
    (void)set_heart_beat(ob, 0);
  new_ob = get_empty_object(ob->prog->p.i.num_variables);
  new_ob->name = make_new_name(ob->name);
  new_ob->flags |= (O_CLONE | (ob->flags & (O_WILL_CLEAN_UP | O_WILL_RESET)));
  new_ob->load_time = ob->load_time;
  new_ob->prog = ob->prog;
  reference_prog(ob->prog, "clone_object");
  if(!current_object)
    fatal("clone_object() from no current_object !\n");
  
  init_object(new_ob);
  
  new_ob->next_all = obj_list;
  obj_list = new_ob;
  enter_object_hash(new_ob);	/* Add name to fast object lookup table */
  reset_object(new_ob, 0); 
  command_giver = save_command_giver;
  /* Never know what can happen ! :-( */
  if(new_ob->flags & O_DESTRUCTED)
    return(0);
  return(new_ob);
}

struct object *environment(arg)
     struct svalue *arg;
{
  struct object *ob = current_object;
  
  if (arg && arg->type == T_OBJECT)
    ob = arg->u.ob;
  if (ob == 0 || ob->super == 0 || (ob->flags & O_DESTRUCTED))
    return 0;
  if (ob->flags & O_DESTRUCTED)
    error("environment() off destructed object.\n");
  return ob->super;
}

/*
 * Execute a command for an object. Copy the command into a
 * new buffer, because 'parse_command()' can modify the command.
 * If the object is not current object, static functions will not
 * be executed. This will prevent forcing users to do illegal things.
 *
 * Return cost of the command executed if success (> 0).
 * When failure, return 0.
 */
int command_for_object(str, ob)
     char *str;
     struct object *ob;
{
  char buff[1000];
  extern int eval_cost;
  int save_eval_cost = eval_cost;
  
  if (strlen(str) > sizeof(buff) - 1)
    error("Too long command.\n");
  if (ob == 0)
    ob = current_object;
  else if (ob->flags & O_DESTRUCTED)
    return 0;
  strncpy(buff, str, sizeof buff);
  buff[sizeof buff - 1] = '\0';
  if (parse_command(buff, ob))
    return eval_cost - save_eval_cost + 1000;  /* why the + 1000 ? */
  else
    return 0;
}

/*
 * To find if an object is present, we have to look in two inventory
 * lists. The first list is the inventory of the current object.
 * The second list is all things that have the same ->super as
 * current_object.
 * Also test the environment.
 * If the second argument 'ob' is non zero, only search in the
 * inventory of 'ob'. The argument 'ob' will be mandatory, later.
 */

static struct object *object_present2 PROT((char *, struct object *));
     
struct object *object_present(v, ob)
     struct svalue *v;
     struct object *ob;
{
  struct svalue *ret;
  struct object *ret_ob;
  int specific = 0;
  
  if (ob == 0)
    ob = current_object;
  else
    specific = 1;
  if (ob->flags & O_DESTRUCTED)
    return 0;
  if (v->type == T_OBJECT) {
    if (specific) {
      if (v->u.ob->super == ob)
	return v->u.ob;
      else
	return 0;
    }
    if (v->u.ob->super == ob ||
	(v->u.ob->super == ob->super && ob->super != 0))
      return v->u.ob->super;
    return 0;
  }
  ret_ob = object_present2(v->u.string, ob->contains);
  if (ret_ob)
    return ret_ob;
  if (specific)
    return 0;
  if (ob->super) {
    push_string(v->u.string, STRING_CONSTANT);
    ret = apply("id", ob->super, 1);
    if (ob->super->flags & O_DESTRUCTED)
      return 0;
    if (!IS_ZERO(ret)) {
      /*
	if id() returns a value of type object then query that object.
	this will allow container objects to allow objects inside them
	to be referred to (for attack or whatever).
	*/
#ifndef OLD_PRESENT
      if (ret->type == T_OBJECT)
	return ret->u.ob;
      else
#endif
	return ob->super;
    }
    return object_present2(v->u.string, ob->super->contains);
  }
  return 0;
}

static struct object *object_present2(str, ob)
     char *str;
     struct object *ob;
{
  struct svalue *ret;
  char *p;
  int count = 0, length;
  char *item;
  
  item = string_copy(str);
  length = strlen(item);
  p = item + length - 1;
  if (*p >= '0' && *p <= '9') {
    while(p > item && *p >= '0' && *p <= '9')
      p--;
    if (p > item && *p == ' ') {
      count = atoi(p+1) - 1;
      *p = '\0';
      length = p - item;	/* This is never used again ! */
    }
  }
  for (; ob; ob = ob->next_inv) {
    push_string(item, STRING_CONSTANT);
    ret = apply("id", ob, 1);
    if (ob->flags & O_DESTRUCTED) {
      FREE(item);
      return 0;
    }
    if (IS_ZERO(ret))
      continue;
    if (count-- > 0)
      continue;
    FREE(item);
#ifndef OLD_PRESENT
    if (ret->type == T_OBJECT)
      return ret->u.ob;
    else
#endif
      return ob;
  }
  FREE(item);
  return 0;
}

/*
 * Remove an object. It is first moved into the destruct list, and
 * not really destructed until later. (see destruct2()).
 */
void destruct_object(v)
     struct svalue *v;
{
  struct object *ob, *super;
  struct object **pp;
  int removed;
  
  if (v->type == T_OBJECT)
    ob = v->u.ob;
  else {
    error("destruct_object: called without an object argument\n");
  }
  /*
   * check if object has an efun socket referencing it for
   * a callback. if so, close the efun socket.
   */
  if (ob->flags & O_EFUN_SOCKET) {
    close_referencing_sockets(ob);
  }
  
  if (ob->flags & O_DESTRUCTED)
    return;
  if (ob->flags & O_SWAPPED)
    load_ob_from_swap(ob);
  remove_object_from_stack(ob);
  /*
   * If this is the first object being shadowed by another object, then
   * destruct the whole list of shadows.
   */
#ifndef NO_SHADOWS 
  if (ob->shadowed && !ob->shadowing) {
    struct svalue svp;
    struct object *ob2;
    
    svp.type = T_OBJECT;
    for (ob2 = ob->shadowed; ob2; ) {
      svp.u.ob = ob2;
      ob2 = ob2->shadowed;
      svp.u.ob->shadowed = 0;
      svp.u.ob->shadowing = 0;
      destruct_object(&svp);
    }
  }
  /*
   * The chain of shadows is a double linked list. Take care to update
   * it correctly.
   */
  if (ob->shadowing)
    ob->shadowing->shadowed = ob->shadowed;
  if (ob->shadowed)
    ob->shadowed->shadowing = ob->shadowing;
  ob->shadowing = 0;
  ob->shadowed = 0;
#endif
  
  if (d_flag > 1)
    debug_message("Destruct object %s (ref %d)\n", ob->name, ob->ref);
  super = ob->super;
  if (super) {
  }
  if (super == 0) {
    /*
     * There is nowhere to move the objects.
     */
    struct svalue svp;
    svp.type = T_OBJECT;
    while(ob->contains) {
      svp.u.ob = ob->contains;
      push_object(ob->contains);
      /* An error here will not leave destruct() in an inconsistent
       * stage.
       */
      apply_master_ob("destruct_environment_of",1);
      if (svp.u.ob == ob->contains)
	destruct_object(&svp);
    }
  } else {
    while(ob->contains)
      move_or_destruct(ob->contains, super);
  }
  add_objects (&ob->stats, -1);
  if ( ob->interactive ) {
    struct object *save=command_giver;
    
    command_giver=ob;
#ifdef ED
    if (ob->interactive->ed_buffer) {
      extern void save_ed_buffer();
      
      save_ed_buffer();
    }
#endif
    command_giver=save;
  }
  set_heart_beat(ob, 0);
  /*
   * Remove us out of this current room (if any).
   * Remove all sentences defined by this object from all objects here.
   */
  if (ob->super) {
    if (ob->super->flags & O_ENABLE_COMMANDS)
      remove_sent(ob, ob->super);
    add_light(ob->super, - ob->total_light);
    for (pp = &ob->super->contains; *pp;) {
      if ((*pp)->flags & O_ENABLE_COMMANDS)
	remove_sent(ob, *pp);
      if (*pp != ob)
	pp = &(*pp)->next_inv;
      else
	*pp = (*pp)->next_inv;
    }
  }
  /*
   * Now remove us out of the list of all objects.
   * This must be done last, because an error in the above code would
   * halt execution.
   */
  removed = 0;
  for (pp = &obj_list; *pp; pp = &(*pp)->next_all) {
    if (*pp != ob)
      continue;
    *pp = (*pp)->next_all;
    removed = 1;
    remove_object_hash(ob);
    break;
  }
  if (!removed)
    fatal("Failed to delete object.\n");
  if (ob->living_name)
    remove_living_name(ob);
  ob->super = 0;
  ob->next_inv = 0;
  ob->contains = 0;
  ob->flags &= ~O_ENABLE_COMMANDS;
  ob->next_all = obj_list_destruct;
  obj_list_destruct = ob;
  ob->flags |= O_DESTRUCTED;
  /* moved this here from destruct2() -- see comments in destruct2() */
  if (ob->interactive) {
    remove_interactive(ob);
  }
}

/*
 * This one is called when no program is executing from the main loop.
 */
void destruct2(ob)
     struct object *ob;
{
  if (d_flag > 1) {
    debug_message("Destruct-2 object %s (ref %d)\n", ob->name, ob->ref);
  }
#if 0
  /* moved this into destruct_object() to deal with the 0 in users() efun
     output problem
     */
  if (ob->interactive)
    remove_interactive(ob);
#endif
  /*
   * We must deallocate variables here, not in 'free_object()'.
   * That is because one of the local variables may point to this object,
   * and deallocation of this pointer will also decrease the reference
   * count of this object. Otherwise, an object with a variable pointing
   * to itself, would never be freed.
   * Just in case the program in this object would continue to
   * execute, change string and object variables into the number 0.
   */
  if (ob->prog->p.i.num_variables > 0) {
    /*
     * Deallocate variables in this object.
     * The space of the variables are not deallocated until
     * the object structure is freed in free_object().
     */
    int i;
    for (i=0; (unsigned)i<ob->prog->p.i.num_variables; i++) {
      free_svalue(&ob->variables[i]);
      ob->variables[i].type = T_NUMBER;
      ob->variables[i].u.number = 0;
      ob->variables[i].subtype = T_NULLVALUE;
    }
  }
  free_object(ob, "destruct_object");
}


/*
 * A message from an object will reach
 * all objects in the inventory,
 * all objects in the same environment and
 * the surrounding object.
 * Non interactive objects gets no messages.
 *
 * There are two cases to take care of. If this routine is called from
 * a user (indirectly), then the message goes to all in his
 * environment. Otherwise, the message goes to all in the current_object's
 * environment (as the case when called from a heart_beat()).
 *
 * If there is a second argument 'avoid_ob', then do not send the message
 * to that object.
 */

void say(v, avoid)
     struct svalue *v;
     struct vector *avoid;
{
  extern struct vector *order_alist PROT((struct vector *));
  struct vector *vtmpp;
  static struct vector vtmp = { 1, 1,
#ifdef DEBUG
				  1,
#endif
                                  {(mudlib_stats_t *)NULL,
                                     (mudlib_stats_t *)NULL},
				  { { T_POINTER } }
			      };
  
  extern int assoc PROT((struct svalue *key, struct vector *));
  struct object *ob, *save_command_giver = command_giver;
  struct object *origin;
  char buff[LARGEST_PRINTABLE_STRING];
#define INITAL_MAX_RECIPIENTS 50 /* change from 3.1.1 */
  int max_recipients = INITAL_MAX_RECIPIENTS;
  struct object **first_recipients;
  struct object **recipients;
  struct object **curr_recipient;
  struct object **last_recipients;
  
  struct object *save_again;
  static struct svalue stmp = { T_OBJECT };
  
  curr_recipient = recipients = first_recipients =
    (struct object **)
	DMALLOC(max_recipients * sizeof(struct object *), 98, "say: 1");
  last_recipients = first_recipients + max_recipients - 1;
  if (current_object->flags & O_ENABLE_COMMANDS)
    command_giver = current_object;
#ifndef NO_SHADOWS 
  else if (current_object->shadowing)
    command_giver = current_object->shadowing;
#endif
  if (command_giver) {
    origin = command_giver;
    if (avoid->item[0].type == T_NUMBER) {
      avoid->item[0].type = T_OBJECT;
      avoid->item[0].u.ob = command_giver;
      add_ref(command_giver, "ass to var");
    }
  } else
    origin = current_object;
  vtmp.item[0].u.vec = avoid;
  vtmpp = order_alist(&vtmp);
  avoid = vtmpp->item[0].u.vec;
  if ((ob = origin->super)) {
    if (ob->flags & O_ENABLE_COMMANDS || ob->interactive) {
      *curr_recipient++ = ob;
    }
    for (ob = origin->super->contains; ob; ob = ob->next_inv) {
      if (ob->flags & O_ENABLE_COMMANDS || ob->interactive) {
	if (curr_recipient >= last_recipients) {
	  max_recipients <<= 1;
	  curr_recipient = (struct object **)
	    DREALLOC(recipients, max_recipients * sizeof(struct object *),
		99, "say: 2");
	  recipients = curr_recipient;
	  last_recipients = &recipients[max_recipients-1];
	}
	*curr_recipient++ = ob;
      }
    }
  }
  for (ob = origin->contains; ob; ob = ob->next_inv) {
    if (ob->flags & O_ENABLE_COMMANDS || ob->interactive) {
      if (curr_recipient >= last_recipients) {
	max_recipients <<= 1;
	curr_recipient = (struct object **)
    DREALLOC(recipients, max_recipients * sizeof(struct object *), 100,
		"say: 3");
	recipients = curr_recipient;
	last_recipients = &recipients[max_recipients-1];
      }
      *curr_recipient++ = ob;
    }
  }
  *curr_recipient = (struct object *)0;
  switch(v->type) {
  case T_STRING:
    strncpy(buff, v->u.string, sizeof buff);
    buff[sizeof buff - 1] = '\0';
    break;
  case T_OBJECT:
    strncpy(buff, v->u.ob->name, sizeof buff);
    buff[sizeof buff - 1] = '\0';
    break;
  case T_NUMBER:
    sprintf(buff, "%d", v->u.number);
    break;
  case T_REAL:
    sprintf(buff, "%f", v->u.real);
    break;
  default:
    error("Invalid argument %d to say()\n", v->type);
  }
  save_again = command_giver;
  for (curr_recipient = recipients; (ob = *curr_recipient++); ) {
    if (ob->flags & O_DESTRUCTED) continue;
    stmp.u.ob = ob;
    if (assoc(&stmp, avoid) >= 0) continue;
    tell_object(ob, buff);
  }
  free_vector(vtmpp);
  FREE((void *)recipients);
  command_giver = save_command_giver;
}

/*
 * Send a message to all objects inside an object.
 * Non interactive objects gets no messages.
 * Compare with say().
 */

void tell_room(room, v, avoid)
     struct object *room;
     struct svalue *v;
     struct vector *avoid; /* has to be in alist order */
{
  struct object *ob;
  char buff[LARGEST_PRINTABLE_STRING];
  
  switch(v->type) {
  case T_STRING:
    strncpy(buff, v->u.string, sizeof buff);
    buff[sizeof buff - 1] = '\0';
    break;
  case T_OBJECT:
    strncpy(buff, v->u.ob->name, sizeof buff);
    buff[sizeof buff - 1] = '\0';
    break;
  case T_NUMBER:
    sprintf(buff, "%d", v->u.number);
    break;
  case T_REAL:
    sprintf(buff, "%f", v->u.real);
    break;
  default:
    error("Invalid argument %d to tell_room()\n", v->type);
  }
  for (ob = room->contains; ob; ob = ob->next_inv) {
    int assoc PROT((struct svalue *key, struct vector *));
    static struct svalue stmp = { T_OBJECT, } ;
    
    stmp.u.ob = ob;
    if (assoc(&stmp, avoid) >= 0) continue;
    if (ob->interactive == 0) {
      if (ob->flags & O_ENABLE_COMMANDS) {
	tell_npc(ob, buff);
      }
      if (ob->flags & O_DESTRUCTED)
	break;
      continue;
    }
    tell_object(ob, buff);
  }
}

void shout_string(str)
     char *str;
{
  struct object *ob;
#ifdef LOG_SHOUT
  FILE *f = 0;
  char *tmpstr;
#endif
  char *p;
  
  str = string_copy(str);	/* So that we can modify the string */
  for (p=str; *p; p++) {
    if ((*p < ' ' || *p > '~') && *p != '\n')
      *p = ' ';
  }
  
  p = 0;
#ifdef LOG_SHOUT
  if (command_giver) {
    struct svalue *v;
    v = apply("query_cap_name", command_giver, 0);
    if (v && v->type == T_STRING)
      p = v->u.string;
    else {
      v = apply("query_name", command_giver, 0);
      if (v && v->type == T_STRING)
	p = v->u.string;
    }
  } else if (current_object && current_object->uid)
    p = current_object->uid->name;
  if (p)
    {
      tmpstr = (char *)DMALLOC(strlen(LOG_DIR) + 8, 101, "shout_string: 1");
      sprintf(tmpstr,"%s/shouts",LOG_DIR);
      if (tmpstr[0] == '/')
	strcpy (tmpstr, tmpstr+1);
      f = fopen(tmpstr, "a");
      FREE(tmpstr);
    }
  if (f) {
    fprintf(f, "%s: %s\n", p, str);
    fclose(f);
  }
#endif
  for (ob = obj_list; ob; ob = ob->next_all) {
    if (!(ob->flags & O_ENABLE_COMMANDS) || (ob == command_giver)
	|| !ob->super)
      continue;
    tell_object(ob,str);
  }
  FREE(str);
}

struct object *first_inventory(arg)
     struct svalue *arg;
{
  struct object *ob;
  
  if (arg->type == T_STRING)
    {
      ob = find_object(arg->u.string);
      if (ob && !object_visible(ob)) ob = 0;
    }
  else
    ob = arg->u.ob;
  if (ob == 0)
    error("No object to first_inventory()");
  ob = ob->contains;
  while (ob)
    {
      if (ob->flags & O_HIDDEN)
        {
	  if (object_visible(ob))
            {
	      return ob;
            }
        } else return ob;
      ob = ob->next_inv;
    }
  return 0;
}

/*
 * This will enable an object to use commands normally only
 * accessible by interactive users.
 * Also check if the user is a wizard. Wizards must not affect the
 * value of the wizlist ranking.
 */

void enable_commands(num)
     int num;
{
  if (current_object->flags & O_DESTRUCTED)
    return;
  if (d_flag > 1) {
    debug_message("Enable commands %s (ref %d)\n",
		  current_object->name, current_object->ref);
  }
  if (num) {
    current_object->flags |= O_ENABLE_COMMANDS;
    command_giver = current_object;
  } else {
    current_object->flags &= ~O_ENABLE_COMMANDS;
    command_giver = 0;
  }
}

/*
 * Set up a function in this object to be called with the next
 * user input string.
 */
int input_to(fun, flag, num_arg, args)
     char *fun;
     int flag;
     int num_arg;
     struct svalue *args;
{
  struct sentence *s;
  struct svalue *x;
  
  if(!command_giver || command_giver->flags & O_DESTRUCTED)
    return 0;
  /*
   * If we have args, we have to copy them, so the svalues on the
   * stack can be freed, which will be done automatically.
   */
  if(num_arg){
    if((x = (struct svalue *)
	DMALLOC(num_arg * sizeof(struct svalue), 102, "input_to: 1")) == NULL)
      fatal("Not enough memory to copy args from input_to.\n");	 
    copy_some_svalues(x, args, num_arg);
  }
  else
    x = NULL;
  s = alloc_sentence();
  if(set_call(command_giver, s, flag, 0)){
    command_giver->interactive->carryover = x;
    command_giver->interactive->num_carry = num_arg;
    s->function = make_shared_string(fun);
    s->ob = current_object;
    add_ref(current_object, "input_to");
    return 1;
  }
  if(x)
    FREE(x);
  free_sentence(s);
  return 0;
}

/*
 * Set up a function in this object to be called with the next
 * user input character.
 */
int get_char (fun, flag)
     char *fun;
     int flag;
{
  struct sentence *s;
  
  if (!command_giver || command_giver->flags & O_DESTRUCTED)
    return 0;
  s = alloc_sentence();
  if (set_call(command_giver, s, flag, 1)) {
    s->function = make_shared_string(fun);
    s->ob = current_object;
    add_ref(current_object, "get_char");
    return 1;
  }
  free_sentence(s);
  return 0;
}

INLINE void
  check_legal_string(s)
char *s;
{
  if (strlen(s) >= LARGEST_PRINTABLE_STRING) {
    error("Printable strings limited to length of %d.\n",
	  LARGEST_PRINTABLE_STRING);
  }
}

void print_svalue(arg)
     struct svalue *arg;
{
  if (arg == 0) {
    add_message("<NULL>");
  } else if (arg->type == T_STRING) {
      check_legal_string(arg->u.string);
      tell_object(command_giver, arg->u.string);
  }
  else if(arg->type == T_OBJECT)
    add_message("OBJ(%s)", arg->u.ob->name);
  else if(arg->type == T_NUMBER)
    add_message("%d", arg->u.number);
  else if(arg->type == T_REAL)
    add_message("%g", arg->u.real);
  else if(arg->type & T_POINTER)
    add_message("<ARRAY>");
  else if (arg->type == T_MAPPING)
    add_message("<MAPPING>");
  else if (arg->type == T_FUNCTION)
    add_message("<FUNCTION>");
  else
    add_message("<UNKNOWN>");
}

void do_write(arg)
     struct svalue *arg;
{
  struct object *save_command_giver = command_giver;
#ifndef NO_SHADOWS
  if(command_giver == 0 && current_object->shadowing)
    command_giver = current_object;
  if(command_giver){
    /* Send the message to the first object in the shadow list */
    while(command_giver->shadowing)
      command_giver = command_giver->shadowing;
  }
#else
  if(!command_giver)
    command_giver = current_object;
#endif /* NO_SHADOWS */
  print_svalue(arg);
  command_giver = save_command_giver;
}

/* Find an object. If not loaded, load it !
 * The object may selfdestruct, which is the only case when 0 will be
 * returned.
 */

struct object *find_object(str)
     char *str;
{
  struct object *ob;
  char *p;
  
  /* don't allow consecutive "/"'s - Wayfarer */
  p = str;
  while (*p)
    {
      if (*p == '/' && *(p + 1) == '/')
	return 0; 
      p++;
    }
  
  /* Remove leading '/' if any. */
  while(str[0] == '/')
    str++;
  ob = find_object2(str);
  if (ob)
    return ob;
  ob = load_object(str, 0);
  if (!ob || (ob->flags & O_DESTRUCTED))		/* *sigh* */
    return 0;
  if (ob && ob->flags & O_SWAPPED)
    load_ob_from_swap(ob);
  return ob;
}

#define MAX_OBJECT_NAME_SIZE 2048

/* Look for a loaded object. Return 0 if non found. */
struct object *find_object2(str)
     char *str;
{
  register struct object *ob;
  register int length;
  char p[MAX_OBJECT_NAME_SIZE];
  
  /* Remove leading '/' if any. */
  while(str[0] == '/')
    str++;
  /* Truncate possible .c in the object name. */
  length = strlen(str);
  if (str[length-2] == '.' && str[length-1] == 'c') {
    /* A new writeable copy of the name is needed. */
    strncpy(p, str, MAX_OBJECT_NAME_SIZE);
    str = p;
    str[length-2] = '\0';
  }
  if ((ob = lookup_object_hash(str))) {
    if (ob->flags & O_SWAPPED)
      load_ob_from_swap(ob);
    return ob;
  }
  return 0;
}



/*
 * Transfer an object.
 * The object has to be taken from one inventory list and added to another.
 * The main work is to update all command definitions, depending on what is
 * living or not. Note that all objects in the same inventory are affected.
 *
 * There are some boring compatibility to handle. When -o flag is specified,
 * several functions are called in some objects. This is dangerous, as
 * object might self-destruct when called.
 */
void move_object(item, dest)
     struct object *item, *dest;
{
  struct object **pp, *ob, *next_ob;
  struct object *save_cmd = command_giver;
  
  if (item != current_object)
    error("Illegal to move other object than this_object()\n");
  /* Recursive moves are not allowed. */
  for (ob = dest; ob; ob = ob->super)
    if (ob == item)
      error("Can't move object inside itself.\n");
#ifndef NO_SHADOWS
  if (item->shadowing)
    error("Can't move an object that is shadowing.\n");
#endif
  
#ifdef LAZY_RESETS
  try_reset(dest);
#endif
  add_light(dest, item->total_light);
  if (item->super) {
    int okey = 0;
    
    if (item->flags & O_ENABLE_COMMANDS) {
      remove_sent(item->super, item);
    }
    if (item->super->flags & O_ENABLE_COMMANDS)
      remove_sent(item, item->super);
    add_light(item->super, - item->total_light);
    for (pp = &item->super->contains; *pp;) {
      if (*pp != item) {
	if ((*pp)->flags & O_ENABLE_COMMANDS)
	  remove_sent(item, *pp);
	if (item->flags & O_ENABLE_COMMANDS)
	  remove_sent(*pp, item);
	pp = &(*pp)->next_inv;
	continue;
      }
      *pp = item->next_inv;
      okey = 1;
    }
    if (!okey)
      fatal("Failed to find object %s in super list of %s.\n",
	    item->name, item->super->name);
  }
  item->next_inv = dest->contains;
  dest->contains = item;
  item->super = dest;
  /*
   * Setup the new commands. The order is very important, as commands
   * in the room should override commands defined by the room.
   * Beware that init() in the room may have moved 'item' !
   *
   * The call of init() should really be done by the object itself
   * (except in the -o mode). It might be too slow, though :-(
   */
  if (item->flags & O_ENABLE_COMMANDS) {
    command_giver = item;
    (void)apply("init", dest, 0);
    if ((dest->flags & O_DESTRUCTED) || item->super != dest) {
      command_giver = save_cmd; /* marion */
      return;
    }
  }
  /*
   * Run init of the item once for every present user, and
   * for the environment (which can be a user).
   */
  for (ob = dest->contains; ob; ob=next_ob) {
    next_ob = ob->next_inv;
    if (ob == item)
      continue;
    if (ob->flags & O_DESTRUCTED)
      error("An object was destructed at call of init()\n");
    if (ob->flags & O_ENABLE_COMMANDS) {
      command_giver = ob;
      (void)apply("init", item, 0);
      if (dest != item->super) {
	command_giver = save_cmd; /* marion */
	return;
      }
    }
    if (item->flags & O_DESTRUCTED) /* marion */
      error("The object to be moved was destructed at call of init()\n");
    if (item->flags & O_ENABLE_COMMANDS) {
      command_giver = item;
      (void)apply("init", ob, 0);
      if (dest != item->super) {
	command_giver = save_cmd; /* marion */
	return;
      }
    }
  }
  if (dest->flags & O_DESTRUCTED) /* marion */
    error("The destination to move to was destructed at call of init()\n");
  if (dest->flags & O_ENABLE_COMMANDS) {
    command_giver = dest;
    (void)apply("init", item, 0);
  }
  command_giver = save_cmd;
}


/*
 * Every object as a count of number of light sources it contains.
 * Update this.
 */

void add_light(p, n)
     struct object *p;
     int n;
{
  if (n == 0)
    return;
  p->total_light += n;
  if (p->super)
    add_light(p->super, n);
}

struct sentence *sent_free = 0;
int tot_alloc_sentence;

struct sentence *alloc_sentence() {
  struct sentence *p;
  
  if (sent_free == 0) {
    p = (struct sentence *)DXALLOC(sizeof *p, 103, "alloc_sentence");
    tot_alloc_sentence++;
  } else {
    p = sent_free;
    sent_free = sent_free->next;
  }
  p->verb = 0;
  p->function = 0;
  p->next = 0;
  return p;
}

#ifdef free
void free_all_sent() {
  struct sentence *p;
  for (;sent_free; sent_free = p) {
    p = sent_free->next;
    FREE(sent_free);
  }
}
#endif

void free_sentence(p)
     struct sentence *p;
{
  if (p->function)
    free_string(p->function);
  p->function = 0;
  if (p->verb)
    free_string(p->verb);
  p->verb = 0;
  p->next = sent_free;
  sent_free = p;
}

/*
 * Find the sentence for a command from the user.
 * Return success status.
 */

int user_parser(buff)
     char *buff;
{
  struct sentence *s;
  char *p;
  int length;
  struct object *save_current_object = current_object,
  *save_command_giver = command_giver;
  char verb_copy[SMALL_STRING_SIZE];
  
  if (d_flag > 1)
    debug_message("cmd [%s]: %s\n", command_giver->name, buff);
  /* strip trailing spaces. */
  for (p = buff + strlen(buff) - 1; p >= buff; p--) {
    if (*p != ' ')
      break;
    *p = '\0';
  }
  if (buff[0] == '\0')
    return 0;
  p = strchr(buff, ' ');
  if (p == 0)
    length = strlen(buff);
  else
    length = p - buff;
  clear_notify();
  s = save_command_giver->sent;
  for ( ; s; s = s->next) {
    struct svalue *ret;
    int len;
    struct object *command_object;
    if (s->verb == 0)
      error("No action linked to verb.\n");
    len = strlen(s->verb);
    if (s->flags & (V_NOSPACE | V_SHORT)) {
      if(strncmp(buff, s->verb,len) != 0)
	      continue;
    } else {
      if (len != length) continue;
      if (strncmp(buff, s->verb, length))
	continue;
    }
    /*
     * Now we have found a special sentence !
     */
    if (d_flag > 1)
      debug_message("Local command %s on %s\n",
		    s->function, s->ob->name);
    if (length >= sizeof verb_copy)
      len = sizeof verb_copy - 1;
    else
      len = length;
    strncpy(verb_copy, buff, len);
    verb_copy[len] = '\0';
    last_verb = verb_copy;
    /*
     * If the function is static and not defined by current object,
     * then it will fail. If this is called directly from user input,
     * then we set current_object so that static functions are allowed.
     * current_object is reset just after the call to apply().
     */
    if (current_object == 0)
      current_object = s->ob;
    /*
     * Remember the object, to update moves.
     */
    command_object = s->ob;
    if (s->flags & V_NOSPACE) {
      push_constant_string(&buff[strlen(s->verb)]);
      ret = apply(s->function,s->ob, 1);
    } else if (buff[length] == ' ') {
      push_constant_string(&buff[length+1]);
      ret = apply(s->function, s->ob, 1);
    } else {
      ret = apply(s->function, s->ob, 0);
    }
    if (current_object->flags & O_DESTRUCTED) {
      /* If disable_commands() were called, then there is no
       * command_giver any longer.
       */
      if (command_giver == 0)
	return 1;
      s = command_giver->sent;	/* Restart :-( */
    }
    current_object = save_current_object;
    last_verb = 0;
    /* If we get fail from the call, it was wrong second argument. */
    if (ret && ret->type == T_NUMBER && ret->u.number == 0)
      continue;
    if (!command_giver) {
      return 1;
    }
    if (s && command_giver->interactive &&
	!(command_giver->flags & O_IS_WIZARD))
      add_moves (&command_object->stats, 1);
    if (ret == 0)
      add_message("Error: action %s not found.\n", s->function);
    return 1;
  }
  notify_no_command();
  return 0;
}

/*
 * Associate a command with function in this object.
 * The optional second argument is the command name. If the command name
 * is not given here, it should be given with add_verb().
 *
 * The optinal third argument is a flag that will state that the verb should
 * only match against leading characters.
 *
 * The object must be near the command giver, so that we ensure that the
 * sentence is removed when the command giver leaves.
 *
 * If the call is from a shadow, make it look like it is really from
 * the shadowed object.
 */
void add_action(str, cmd, flag)
     char *str, *cmd;
     int flag;
{
  struct sentence *p;
  struct object *ob;
  
  if (str[0] == ':')
    error("Illegal function name: %s\n", str);
  if (current_object->flags & O_DESTRUCTED)
    return;
  ob = current_object;
#ifndef NO_SHADOWS 
  while(ob->shadowing)
    ob = ob->shadowing;
#endif
  if (command_giver == 0 || (command_giver->flags & O_DESTRUCTED))
    return;
  if (ob != command_giver && ob->super != command_giver &&
      ob->super != command_giver->super && ob != command_giver->super)
    return;  /* No need for an error, they know what they did wrong. */
  if (d_flag > 1)
    debug_message("--Add action %s\n", str);
  p = alloc_sentence();
  p->function = make_shared_string(str);
  p->ob = ob;
  p->next = command_giver->sent;
  p->flags = flag;
  if (cmd)
    p->verb = make_shared_string(cmd);
  else
    p->verb = 0;
  command_giver->sent = p;
}

void add_verb(str, flag)
     char *str;
     int flag;
{
  if (command_giver == 0 || (command_giver->flags & O_DESTRUCTED))
    return;
  if (command_giver->sent == 0)
    error("No add_action().\n");
  if (command_giver->sent->verb != 0)
    error("Tried to set verb again.\n");
  command_giver->sent->verb = make_shared_string(str);
  if (flag)
    command_giver->sent->flags |= V_NOSPACE;
  if (d_flag > 1)
    debug_message("--Adding verb %s to action %s\n", str,
		  command_giver->sent->function);
}


/*
 * Remove sentence with specified verb and action.  Return 1
 * if success.  If command_giver, remove his action, otherwise
 * remove current_object's action.
 */
int remove_action(act, verb)
     char *act, *verb;
{
  struct object *ob;
  struct sentence **s;
  if (command_giver)
    ob = command_giver;
  else
    ob = current_object;
  if (!ob) return 0;
  for (s = &ob->sent; *s; ) {
    struct sentence *tmp;
    if (((*s)->ob == ob) && !strcmp((*s)->function, act) &&
	!strcmp((*s)->verb, verb)) {
      tmp = *s;
      *s = tmp->next;
      free_sentence(tmp);
      return 1;
    }
    s = &((*s)->next);   /* Code look familiar? ;) */
  }
  return 0;
}

/*
 * Remove all commands (sentences) defined by object 'ob' in object
 * 'user'
 */
void remove_sent(ob, user)
     struct object *ob, *user;
{
  struct sentence **s;
  
  for (s= &user->sent; *s;) {
    struct sentence *tmp;
    if ((*s)->ob == ob) {
      if (d_flag > 1)
	debug_message("--Unlinking sentence %s\n", (*s)->function);
      tmp = *s;
      *s = tmp->next;
      free_sentence(tmp);
    } else
      s = &((*s)->next);
  }
}

/*VARARGS1*/
void debug_fatal(fmt, a, b, c, d, e, f, g, h)
     char *fmt;
     int a, b, c, d, e, f, g, h;
{
  static int in_fatal = 0;
  /* Prevent double fatal. */
  if (in_fatal)
    abort();
  in_fatal = 1;
  (void)fprintf(stderr, fmt, a, b, c, d, e, f, g, h);
  fflush(stderr);
  if (current_object)
    (void)fprintf(stderr, "Current object was %s\n",
		  current_object->name);
  debug_message(fmt, a, b, c, d, e, f, g, h);
  if (current_object)
    debug_message("Current object was %s\n", current_object->name);
  debug_message("Dump of variables:\n");
  (void)dump_trace(1);
}

void fatal(fmt, a, b, c, d, e, f, g, h)
     char *fmt;
     int a, b, c, d, e, f, g, h;
{
  debug_fatal(fmt, a, b, c, d, e, f, g, h);
#if !defined(DEBUG_NON_FATAL) || !defined(DEBUG)
  abort();
#endif
}

int num_error = 0;

/*
 * Error() has been "fixed" so that users can catch and throw them.
 * To catch them nicely, we really have to provide decent error information.
 * Hence, all errors that are to be caught
 * (error_recovery_context_exists == 2) construct a string containing
 * the error message, which is returned as the
 * thrown value.  Users can throw their own error values however they choose.
 */

/*
 * This is here because throw constructs its own return value; we dont
 * want to replace it with the system's error string.
 */

void throw_error() {
  extern int error_recovery_context_exists;
  extern jmp_buf error_recovery_context;
  if (error_recovery_context_exists > 1) {
    LONGJMP(error_recovery_context, 1);
    fatal("Throw_error failed!");
  }
  error("Throw with no catch.\n");
}

static char emsg_buf[2000];

/*VARARGS1*/
void error(fmt, a, b, c, d, e, f, g, h)
     char *fmt;
     int a, b, c, d, e, f, g, h;
{
  extern int num_objects_this_thread;
  extern int error_recovery_context_exists;
  extern jmp_buf error_recovery_context;
  extern struct object *current_heart_beat;
  extern struct svalue catch_value;
  extern int too_deep_error, max_eval_error;
  char *object_name;
  
  sprintf(emsg_buf+1, fmt, a, b, c, d, e, f, g, h);
  emsg_buf[0] = '*';	/* all system errors get a * at the start */
  num_objects_this_thread = 0; /* reset the count */
  if (error_recovery_context_exists > 1) { /* user catches this error */
    struct svalue v;
    v.type = T_STRING;
    v.u.string = emsg_buf;
    v.subtype = STRING_MALLOC;	/* Always reallocate */
    assign_svalue(&catch_value, &v);
    LONGJMP(error_recovery_context, 1);
    fatal("Catch() longjump failed");
  }
  too_deep_error = max_eval_error = 0;
  num_error++;
  if (num_error > 1)
    fatal("Too many simultaneous errors.\n");
  debug_message("%s", emsg_buf+1);
  if (current_object) {
    add_errors (&current_object->stats, 1);
    debug_message("program: %s, object: %s line %d\n",
		  current_prog ? current_prog->name : "",
		  current_object->name,
		  get_line_number_if_any());
  }
#if defined(DEBUG) && defined(TRACE_CODE)
  object_name = dump_trace(1);
#else
  object_name = dump_trace(0);
#endif
  fflush(stdout);
  if (object_name) {
    struct object *ob;
    ob = find_object2(object_name);
    if (!ob) {
      if (command_giver)
	add_message("error when executing program in destroyed object %s\n",
		    object_name);
      debug_message("error when executing program in destroyed object %s\n",
		    object_name);
    }
  }
  if (command_giver && command_giver->interactive) {
    num_error--;
    /* 
     * The stack must be brought in a usable state. After the
     * call to reset_machine(), all arguments to error() are invalid,
     * and may not be used any more. The reason is that some strings
     * may have been on the stack machine stack, and has been deallocated.
     */
    reset_machine (0);
    num_error++;
    if ((command_giver->flags & O_IS_WIZARD) || !strlen(DEFAULT_ERROR_MESSAGE))
      {
	add_message("%s", emsg_buf+1);
	if (current_object)
	  add_message("program: %s, object: %s line %d\n",
		      current_prog ? current_prog->name : "",
		      current_object->name,
		      get_line_number_if_any());
      } else {
	add_message("%s\n", DEFAULT_ERROR_MESSAGE);
      }
  }
  if (current_heart_beat) {
    set_heart_beat(current_heart_beat, 0);
    debug_message("Heart beat in %s turned off.\n", current_heart_beat->name);
    if (current_heart_beat->interactive) {
      struct object *save_cmd = command_giver;
      command_giver = current_heart_beat;
      add_message("MudOS driver tells you: You have no heart beat!\n");
      command_giver = save_cmd;
    }
    current_heart_beat = 0;
  }
  num_error--;
  if (error_recovery_context_exists)
    LONGJMP(error_recovery_context, 1);
  abort();
}

/*
 * This one is called from HUP.
 */
int MudOS_is_being_shut_down;

#if !defined(_AIX) && !defined(NeXT) && !defined(_SEQUENT_) && !defined(SVR4) \
	&& !defined(cray) && !defined(SunOS_5)
void startshutdownMudOS() {
  MudOS_is_being_shut_down = 1;
}
#else
void startshutdownMudOS(arg)
     int arg;
{
  MudOS_is_being_shut_down = 1;
}
#endif

/*
 * This one is called from the command "shutdown".
 * We don't call it directly from HUP, because it is dangerous when being
 * in an interrupt.
 */
void shutdownMudOS(exit_code)
     int exit_code;
{
  shout_string("MudOS driver shouts: shutting down immediately.\n");
  save_stat_files();
  ipc_remove();
  unlink_swap_file();
#ifdef DEALLOCATE_MEMORY_AT_SHUTDOWN
  remove_all_objects();
  free_all_sent();
  free_mudlib_stats();
  dump_malloc_data();
  find_alloced_data();
#endif
#ifdef PROFILING
  monitor(0,0,0,0,0);  /* cause gmon.out to be written */
#endif
  exit(exit_code);
}

/*
 * Move or destruct one object.
 */
void move_or_destruct(what, to)
     struct object *what, *to;
{
  struct svalue v;
  
  struct svalue *svp;
  /* This is very dubious, why not just destruct them /JnA 
   */
  push_object(to);
  push_number(1);
  svp = apply("move", what, 2);
  if (svp && svp->type == T_NUMBER && svp->u.number == 0)
    return;
  if (what->flags & O_DESTRUCTED)
    return;
  
  /*
   * Failed to move the object. Then, it is destroyed.
   */
  v.type = T_OBJECT;
  v.u.ob = what;
  destruct_object(&v);
}

/*
 * Call this one when there is only little memory left. It will start
 * Armageddon.
 */
void slow_shut_down(minutes)
     int minutes;
{
  /*
   * Swap out objects, and free some memory.
   */
  push_number(minutes);
  if (IS_ZERO(apply_master_ob("slow_shutdown",1)))
    {
      struct object *save_current = current_object,
      *save_command = command_giver;
      command_giver = 0;
      current_object = 0;
      shout_string("MudOS driver shouts: Out of memory.\n");
      command_giver = save_command;
      current_object = save_current;
#if !defined(_AIX) && !defined(NeXT) && !defined(_SEQUENT_) && !defined(SVR4) \
	&& !defined(cray) && !defined(SunOS_5)
      startshutdownMudOS();
#else
      startshutdownMudOS(1);
#endif
      return;
    }
}

void do_message(class, msg, scope, exclude)
     char *class, *msg;
     struct vector *scope, *exclude;
{
  int i, j, valid;
  struct vector *use,*tmp;
  struct object *ob;
  
  use = slice_array(scope,0,scope->size - 1);
  for(i = 0; i < scope->size; i++)
    {
      if(scope->item[i].type == T_STRING)
	{
	  error ("do_message: bad type.\n");
	  ob = find_object(scope->item[i].u.string);
	  if (ob && !object_visible(ob)) ob = 0;
	  if(ob == 0)
            error("message: Couldn't find %s",scope->item[i].u.string);
	  scope->item[i].type = T_OBJECT;
	  scope->item[i].u.ob = ob;
	  add_ref(ob,"message");
	}
      if(!(scope->item[i].u.ob->flags & O_ENABLE_COMMANDS))
	{
	  struct vector *ai;

	  tmp = use;
	  use = add_array(use,ai = all_inventory(scope->item[i].u.ob, 1));
	  free_vector(ai);
	  free_vector(tmp);
	}
    }
  
  for(i = 0; i < use->size; i++)
    {
      if ((use->item[i].u.ob->flags & O_ENABLE_COMMANDS) ||
	  use->item[i].u.ob->interactive)
	{
	  for(valid = 1, j = 0; j < exclude->size; j++)
            if(exclude->item[j].u.ob == use->item[i].u.ob)
	      {
		valid = 0;
		break;
	      }
	  if(valid)
	    {
	      push_string(class,STRING_CONSTANT);
	      push_string(msg,STRING_CONSTANT);
	      apply("receive_message",use->item[i].u.ob,2);
	    }
	}
    }
  free_vector(use);
}

#ifdef LAZY_RESETS
INLINE void
  try_reset(ob)
struct object *ob;
{
  if ((ob->next_reset < current_time) && !(ob->flags & O_RESET_STATE)) {
    if(d_flag) {
      fprintf(stderr, "(lazy) RESET %s\n", ob->name);
    }
    /* need to set the flag here to prevent infinite loops in apply_low */
    ob->flags |= O_RESET_STATE;
    reset_object(ob, 1);
  }
}
#endif

