/*****************************************************************************/
/*    Copyright (C) 2006-2019 cloonix@cloonix.net License AGPL-3             */
/*                                                                           */
/*  This program is free software: you can redistribute it and/or modify     */
/*  it under the terms of the GNU Affero General Public License as           */
/*  published by the Free Software Foundation, either version 3 of the       */
/*  License, or (at your option) any later version.                          */
/*                                                                           */
/*  This program is distributed in the hope that it will be useful,          */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of           */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            */
/*  GNU Affero General Public License for more details.a                     */
/*                                                                           */
/*  You should have received a copy of the GNU Affero General Public License */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*                                                                           */
/*****************************************************************************/
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <libcrcanvas.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "io_clownix.h"
#include "rpc_clownix.h"
#include "doorways_sock.h"
#include "client_clownix.h"
#include "interface.h"
#include "commun_consts.h"
#include "bank.h"
#include "move.h"
#include "popup.h"
#include "cloonix.h"
#include "main_timer_loop.h"
#include "pid_clone.h"
#include "menu_utils.h"
#include "menus.h"
#include "xml_utils.h"
#include "layout_x_y.h"


char *get_doors_client_addr(void);

GtkWidget *get_main_window(void);

void put_top_left_icon(GtkWidget *mainwin);

static char binary_name[MAX_NAME_LEN];
static char g_sav_dir[MAX_PATH_LEN];


/****************************************************************************/
enum {
  type_pid_none=0,
  type_pid_qmonitor,
  type_pid_spicy_gtk,
  type_pid_wireshark,
  type_pid_dtach,
};
typedef struct t_pid_wait
{
  int type;
  int pid;
  char name[MAX_NAME_LEN];
  char **argv;
} t_pid_wait;

/****************************************************************************/
typedef struct t_qemu_spice_item 
{
  char vm_name[MAX_NAME_LEN];
  char doors_server_path[MAX_NAME_LEN];
  char spice_server_path[MAX_PATH_LEN];
  struct t_qemu_spice_item *prev;
  struct t_qemu_spice_item *next;
} t_qemu_spice_item;

static t_qemu_spice_item *qemu_spice_head;




/*****************************************************************************/
static int file_exists_exec(char *path)
{
  int err, result = 0;
  err = access(path, X_OK);
  if (!err)
    result = 1;
  return result;
}
/*---------------------------------------------------------------------------*/

/****************************************************************************/
static t_qemu_spice_item *qemu_spice_find_name(char *vm_name)
{
  t_qemu_spice_item *cur = qemu_spice_head;
  while (cur)
    {
    if (!strcmp(cur->vm_name, vm_name))
      break;
    cur = cur->next;
    }
  return cur;
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void qemu_spice_release_name(char *vm_name)
{
  t_qemu_spice_item *cur = qemu_spice_find_name(vm_name);
  if (!cur)
    KOUT(" ");
  if (cur == qemu_spice_head)
    {
    if (cur->next)
      cur->next->prev = NULL;
    qemu_spice_head = cur->next;
    }
  else
    {
    if (!cur->prev)
      KOUT(" ");
    cur->prev->next = cur->next;
    if (cur->next)
      cur->next->prev = cur->prev; 
    }
  clownix_free(cur, __FUNCTION__);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static t_qemu_spice_item *qemu_spice_reserve_name(char *vm_name)
{
  t_qemu_spice_item *result = NULL;
  t_qemu_spice_item *cur = qemu_spice_find_name(vm_name);
  if (!cur)
    {
    result = (t_qemu_spice_item *)
             clownix_malloc(sizeof(t_qemu_spice_item), 18);
    memset(result, 0, sizeof(t_qemu_spice_item));
    strcpy(result->vm_name, vm_name);
    if (qemu_spice_head)
      {
      qemu_spice_head->prev = result;
      result->next = qemu_spice_head;
      qemu_spice_head = result;
      }
    else
      qemu_spice_head = result;
    }
  return result;
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
static int start_launch(void *ptr)
{
  char **argv = (char **) ptr;
  execvp(argv[0], argv);
  return 0;
}
/*---------------------------------------------------------------------------*/

/****************************************************************************/
static void release_pid_wait(t_pid_wait *pid_wait)
{
  int i;
  for (i=0; pid_wait->argv[i] != NULL; i++)
    clownix_free(pid_wait->argv[i], __FUNCTION__);
  clownix_free(pid_wait->argv, __FUNCTION__);
  clownix_free(pid_wait, __FUNCTION__);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void death_pid_wait_launch(void *data, int status, char *name)
{
  t_pid_wait *pid_wait = (t_pid_wait *) data;
  int pid;
  switch (pid_wait->type)
    {

    case type_pid_qmonitor:
      pid = bank_get_qmonitor_pid(pid_wait->name);
      if (pid == pid_wait->pid)
        bank_set_qmonitor_pid(pid_wait->name, 0);
      break;

    case type_pid_spicy_gtk:
      pid = bank_get_spicy_gtk_pid(pid_wait->name);
      if (pid == pid_wait->pid)
        bank_set_spicy_gtk_pid(pid_wait->name, 0);
      break;

    case type_pid_wireshark:
      pid = bank_get_wireshark_pid(pid_wait->name);
      if (pid == pid_wait->pid)
        bank_set_wireshark_pid(pid_wait->name, 0);
      break;

    case type_pid_dtach:
      pid = bank_get_dtach_pid(pid_wait->name);
      if (pid == pid_wait->pid)
        bank_set_dtach_pid(pid_wait->name, 0);
      break;

    default:
      KOUT("%d", pid_wait->type);
    }
  release_pid_wait(pid_wait);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static char *alloc_argv(char *str)
{
  int len = strlen(str);
  char *argv = (char *)clownix_malloc(len + 1, 15);
  memset(argv, 0, len + 1);
  strncpy(argv, str, len);
  return argv;
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void launch_new_pid(t_pid_wait *pid_wait)
{
  int pid;
  switch (pid_wait->type)
    {

    case type_pid_qmonitor:
      pid = pid_clone_launch(start_launch, death_pid_wait_launch, NULL,
                             (void *)(pid_wait->argv), (void *) pid_wait,
                             NULL, pid_wait->name, -1, 0);
      bank_set_qmonitor_pid(pid_wait->name, pid);
      break;

    case type_pid_spicy_gtk:
      pid = pid_clone_launch(start_launch, death_pid_wait_launch, NULL,
                             (void *)(pid_wait->argv), (void *) pid_wait,
                             NULL, pid_wait->name, -1, 0);
      bank_set_spicy_gtk_pid(pid_wait->name, pid);
      break;

    case type_pid_wireshark:
      pid = pid_clone_launch(start_launch, death_pid_wait_launch, NULL,
                             (void *)(pid_wait->argv), (void *) pid_wait,
                             NULL, pid_wait->name, -1, 0);
      bank_set_wireshark_pid(pid_wait->name, pid);
      break;

    case type_pid_dtach:
      pid = pid_clone_launch(start_launch, death_pid_wait_launch, NULL,
                             (void *)(pid_wait->argv), (void *) pid_wait,
                             NULL, pid_wait->name, -1, 0);
      bank_set_dtach_pid(pid_wait->name, pid);
      break;

    default:
      KOUT(" ");
    }
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void launch_pid_wait(int type, char *nm, char **argv)
{
  t_pid_wait *pid_wait;
  int i, pid;
  switch (type)
    {
    case type_pid_qmonitor:
      pid = bank_get_qmonitor_pid(nm);
      break;
    case type_pid_spicy_gtk:
      pid = bank_get_spicy_gtk_pid(nm);
      break;
    case type_pid_wireshark:
      pid = bank_get_wireshark_pid(nm);
      break;
    case type_pid_dtach:
      pid = bank_get_dtach_pid(nm);
      break;
    default:
      KOUT(" ");
    }
  if (pid)
    pid_clone_kill_single(pid);
  pid_wait = (t_pid_wait *) clownix_malloc(sizeof(t_pid_wait), 8);
  memset(pid_wait, 0, sizeof(t_pid_wait));
  pid_wait->type = type;
  pid_wait->pid = pid;
  strncpy(pid_wait->name, nm, MAX_NAME_LEN-1);
  pid_wait->argv = (char **)clownix_malloc(20 * sizeof(char *), 13);
  memset(pid_wait->argv, 0, 20 * sizeof(char *));
  for (i=0; i<19; i++)
    {
    if (argv[i] == NULL)
      break;
    pid_wait->argv[i] = alloc_argv(argv[i]);
    }
  if (i >= 18)
    KOUT(" ");
  launch_new_pid(pid_wait);
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
static void debug_print_cmd(char **argv)
{
  int i, len = 0;
  char info[MAX_PRINT_LEN];
  for (i=0; argv[i]; i++)
    {
    len += sprintf(info+len, "%s ", argv[i]);
    }
//  KERR("%s", info);
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
int check_before_start_launch(char **argv)
{
  int result = 0;
  char info[MAX_PRINT_LEN];
  if (file_exists_exec(argv[0]))
    result = 1;
  else
    {
    sprintf(info, "File: %s Problem\n", argv[0]);
    insert_next_warning(info, 1);
    }
  debug_print_cmd(argv);
  return result;
}
/*---------------------------------------------------------------------------*/

/****************************************************************************/
static void start_qemu_spice(char *password, char *path, t_qemu_spice_item *it)
{
  char name[MAX_NAME_LEN];
  char net[MAX_NAME_LEN];
  char *argv[] = { "cloonix_ice", net, name, NULL }; 
  memset(name, 0, MAX_NAME_LEN);
  memset(net, 0, MAX_NAME_LEN);
  snprintf (name, MAX_NAME_LEN, "%s", it->vm_name);
  snprintf (net, MAX_NAME_LEN, "%s", local_get_cloonix_name());
  name[MAX_NAME_LEN-1] = 0;
  net[MAX_NAME_LEN-1] = 0;
  launch_pid_wait(type_pid_spicy_gtk, it->vm_name, argv);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void start_wireshark(char *name, t_bank_item *bitem)
{
  char bin_path[MAX_PATH_LEN];
  char config[MAX_PATH_LEN];
  char cloonix_name[MAX_NAME_LEN];
  char *recpath = bitem->pbi.pbi_sat->topo_snf.recpath;
  char *argv[]={bin_path, config, cloonix_name, "-dae",
               "/usr/bin/wireshark", "-r", recpath, NULL}; 
  memset(bin_path, 0, MAX_PATH_LEN);
  memset(config, 0, MAX_PATH_LEN);
  memset(cloonix_name, 0, MAX_NAME_LEN);

  snprintf(bin_path, MAX_PATH_LEN-1,
                     "%s/client/xwycli/xwycli", get_local_cloonix_tree());

  snprintf(config, MAX_PATH_LEN-1,
                     "%s/cloonix_config", get_local_cloonix_tree());

  snprintf(cloonix_name, MAX_NAME_LEN-1, "%s", local_get_cloonix_name());

  if (check_before_start_launch(argv))
    launch_pid_wait(type_pid_wireshark, name, argv);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void start_local_wireshark(char *name, t_bank_item *bitem)
{
  char bin_path[MAX_PATH_LEN];
  char rec[MAX_PATH_LEN];
  char *argv[] = {bin_path, "-r", rec, NULL};
  memset(bin_path, 0, MAX_PATH_LEN);
  memset(rec, 0, MAX_PATH_LEN);
  snprintf(bin_path, MAX_PATH_LEN, "wireshark");
  snprintf(rec, MAX_PATH_LEN, "%s", bitem->pbi.pbi_sat->topo_snf.recpath);
  bin_path[MAX_PATH_LEN-1] = 0;
  rec[MAX_PATH_LEN-1] = 0;
  if (check_before_start_launch(argv))
    launch_pid_wait(type_pid_wireshark, name, argv);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void node_qemu_spice(GtkWidget *mn, t_item_ident *pm)
{
  t_qemu_spice_item *it;
  char info[MAX_PRINT_LEN];
  char *path = get_path_to_qemu_spice();
  int vm_id;
  if (path)
    {
    it = qemu_spice_reserve_name(pm->name);
    if (it)
      {
      vm_id = get_vm_id_from_topo(pm->name);
      if (!vm_id)
        {
        sprintf(info, "Could not get vm_id");
        insert_next_warning(info, 1);
        }
      else
        {
        snprintf(it->spice_server_path, MAX_PATH_LEN-1, "%s", 
                                        get_spice_vm_path(vm_id));
        snprintf(it->doors_server_path, MAX_NAME_LEN-1, "%s", 
                                        get_doors_client_addr()); 
        start_qemu_spice(get_password(), path, it);
        }
      qemu_spice_release_name(pm->name);
      }
    else
      {
      sprintf(info, "Problem qemu_spice_reserve_name");
      insert_next_warning(info, 1);
      }
    }
  else
    {
    sprintf(info, 
            "Missing:       %s/common/spice/spice_lib/bin/spicy", 
            get_local_cloonix_tree());
    insert_next_warning(info, 1);
    }
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void node_dtach_console(GtkWidget *mn, t_item_ident *pm)
{
  char *nm = pm->name;
  char **argv = get_argv_local_xwy(nm);
  if (check_before_start_launch(argv))
    launch_pid_wait(type_pid_dtach, nm, argv);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void node_xterm_qmonitor(GtkWidget *mn, t_item_ident *pm)
{
  static char title[2*MAX_NAME_LEN];
  static char nm[MAX_NAME_LEN];
  static char cmd[MAX_PATH_LEN];
  static char xvt[MAX_PATH_LEN];
  char *argv[] = {xvt, "-T", title, "-e", "/bin/bash", "-c", cmd, NULL};
  memset(title, 0, 2*MAX_NAME_LEN);
  memset(nm, 0, MAX_NAME_LEN);
  memset(cmd, 0, MAX_PATH_LEN);
  cloonix_get_xvt(xvt);
  strncpy(nm, pm->name, MAX_NAME_LEN-1);
  snprintf(title, 2*MAX_NAME_LEN-1, "%s/%s", local_get_cloonix_name(), nm);
  sprintf(cmd, "%s/client/qmonitor/qmonitor %s %s %s; sleep 5", 
               get_distant_cloonix_tree(), get_doors_client_addr(),
               get_password(), nm); 
  if (check_before_start_launch(argv))
    launch_pid_wait(type_pid_qmonitor, nm, argv);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void timer_end_del_all(void *data)
{
  look_for_lan_and_del_all();
}
/*--------------------------------------------------------------------------*/


/****************************************************************************/
static void callback_sav_all(int tid, int status, char *err)
{
  char info[MAX_PRINT_LEN];
  if (status)
    {
    sprintf(info, "Save all ended KO: %s\n", err);
    insert_next_warning(info, 1);
    }
  else
    {
    sprintf(info, "Save all ended OK\n");
    insert_next_warning(info, 1);
    }
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
static void callback_del_all(int tid, int status, char *err)
{
  char info[MAX_PRINT_LEN];
  if (status)
    {
    sprintf(info, "Del all ended KO: %s\n", err);
    insert_next_warning(info, 1);
    }
  else
    clownix_timeout_add(1, timer_end_del_all, NULL, NULL, NULL);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void topo_delete(GtkWidget *mn)
{
  client_del_all(0, callback_del_all);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void topo_save(GtkWidget *mn)
{
  int response;
  char *tmp;
  GtkWidget *entry_rootfs, *parent, *dialog;
  parent = get_main_window();
  dialog = gtk_dialog_new_with_buttons ("Path of directory",
                                        GTK_WINDOW (parent),
                                        GTK_DIALOG_MODAL,
                                        "_OK",
                                        GTK_RESPONSE_ACCEPT,
                                        "_Cancel",
                                        GTK_RESPONSE_REJECT,
                                        NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 20);
  entry_rootfs = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry_rootfs), g_sav_dir);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                     entry_rootfs, TRUE, TRUE, 0);
  gtk_widget_show_all(dialog);
  response = gtk_dialog_run(GTK_DIALOG (dialog));
  if (response == GTK_RESPONSE_ACCEPT)
    {
    tmp = (char *) gtk_entry_get_text(GTK_ENTRY(entry_rootfs));
    memset(g_sav_dir, 0, MAX_PATH_LEN);
    strncpy(g_sav_dir, tmp, MAX_PATH_LEN-1);
    if (tmp && strlen(tmp))
      {
      client_sav_vm_all(0, callback_sav_all, 0, tmp);
      }
    }
  gtk_widget_destroy(dialog);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
void menu_utils_init(void)
{
  qemu_spice_head = NULL;
  strcpy(binary_name, "cloonix_tux2tux");
  memset(g_sav_dir, 0, MAX_PATH_LEN);
  snprintf(g_sav_dir, MAX_PATH_LEN-1, "%s/replay", getenv("HOME"));

}
/*--------------------------------------------------------------------------*/
