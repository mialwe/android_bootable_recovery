/* Copyright (C) 2010 Zsolt Sz Sztupák
 * Modified by HardCORE (Rodney Clinton Chua)
 * Modified and renamed (orig. lagfixutils.c) by mialwe (Michael Weingärtner)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "midnight.h"
#include "nandroid.h"

extern char **environ;

void apply_ln(char* name) {
  char tmp[128];
  sprintf(tmp,"ln -s /system/xbin/busybox /system/xbin/%s",name);
  __system(tmp);
}

void remove_ln(char* name) {
  char tmp[128];
  sprintf(tmp,"rm /system/xbin/%s",name);
  __system(tmp);
}

void apply_rm(char* name) {
  char tmp[128];
  sprintf(tmp,"/system/xbin/rm /system/bin/%s",name);
  __system(tmp);
}

int get_partition_free(const char *partition){
    if (ensure_path_mounted(partition) != 0){
        ui_print("Can't mount %s\n",partition);
        return -1;
    }
    int ret;
    struct statfs s;
    if (0 != (ret = statfs(partition, &s))){
        ui_print("Unable to stat %s\n",partition);
        return(-1);}
    uint64_t bavail = s.f_bavail;
    uint64_t bsize = s.f_bsize;
    uint64_t freecalc = bavail * bsize;
    uint64_t free_mb = freecalc / (uint64_t)(1024 * 1024);
    return free_mb;
    }

int file_exists(const char *filename){
    char tmp[PATH_MAX];
    struct stat file_info;
    sprintf(tmp, "%s", filename);
    return(statfs(tmp, &file_info));
}

int show_file_exists(const char *pre, const char *filename, const char *ui_filename, const char *post, const char *post_no){
    if(0 == file_exists(filename)){
        ui_print("%s %s %s\n",pre,ui_filename,post);
        return 1; 
        }
    ui_print("%s %s %s\n",pre,ui_filename,post_no);
    return 0;
    }

void custom_menu(
    const char* headers[],
    const char* list[],
    const int numtweaks,
    const char* options[],
    const char* conffile,
    int onlyone
    ) {
    int tweaks[numtweaks];
    int i;
    char buf[128];
    ensure_path_mounted("/data");
    for (;;)
    {
        for (i=0;i<numtweaks;i++) tweaks[i]=0;          // set all tweaks disabled
        FILE* f = fopen(conffile,"r");                  // open configfile
        ui_print("\n\nEnabled options:\n");
        if (f) {
          while (fgets(buf,127,f)) {                    // read all enabled options
            ui_print("%s",buf);
            if (onlyone!=1){                            // only if multiple options possible:
                for (i=0; i<numtweaks; i++) {           // enable options in tweaks array
                    if (memcmp(buf,options[i],strlen(options[i]))==0) tweaks[i]=1;
                }
            }
          }
          fclose(f);
        }                                               // done
        
        // start menu
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        tweaks[chosen_item] = tweaks[chosen_item]?0:1;  // toggle selected option
        f = fopen(conffile,"w+");                       // write options to config file
        if (f) {
          for (i=0; i<numtweaks; i++) {
            if (tweaks[i]) fprintf(f,"%s\n",options[i]);
          }
          fclose(f);
        } else {
          ui_print("Could not create config file\n");
        }
    }
    ensure_path_unmounted("/data");
}

int apply_zipalign(const char* directory)
{
    char path[PATH_MAX] = "";
    char tempdir[PATH_MAX] = "";
    char checkit[PATH_MAX] = "/res/misc/zipalign -c 4 %s";
    char doit[PATH_MAX] = "/res/misc/zipalign -f 4 %s /sdcard/midnight_zipalign%s";
    char copyit[PATH_MAX] = "cp -p /sdcard/midnight_zipalign%s %s";
    char removeit[PATH_MAX] = "rm /sdcard/midnight_zipalign%s";
    char chmodit[PATH_MAX] = "chmod 644 %s";
    char *fname = NULL;
    char cmd_zipalign[PATH_MAX] ="";
    int numFiles = 0;
    int i = 0;
    int zcount = 0;
    unsigned int freemb = 0;
    unsigned int maxmb = 0;
    struct stat filedata;
        
    ui_print("\nStarting zipaligning in %s\n",directory);

    // we need our source packages...
    if(strstr(directory,"/data") !=0 ){
        LOGI("/data zipalign requested, mounting...\n");
        if(0 != ensure_path_mounted("/data")){
            ui_print("Failed to mount /data, exiting...\n");
            return 1;
        }
    }
    if(strstr(directory,"/system") !=0 ){
        LOGI("/system zipalign requested, mounting...\n");
        if(0 != ensure_path_mounted("/system")){
            ui_print("Failed to mount /system, exiting...\n");
            return 1;
        }
    }    
    // we need temp space to zipalign...
    if(0 != ensure_path_mounted("/sdcard")){
        ui_print("Failed to mount /sdcard, exiting...\n");
        return 1;
    }
            
    // get files
    char** files = gather_files(directory,".apk", &numFiles);
    // bail out if nothing found
    if (numFiles <= 0)
    {
        ui_print("No files found, exiting.\n");
        return 1;
    }

    // create temp directory to zipalign...   
    ui_print("Creating temp dir /sdcard/midnight_zipalign...\n");
    sprintf(tempdir,"mkdir -p /sdcard/midnight_zipalign%s",directory); 
    if(0 != __system(tempdir)){
        ui_print("Failed to to execute %s, exiting...\n",tempdir);
        return 1;
    }
    
    char** list = (char**) malloc((numFiles + 1) * sizeof(char*));
    list[numFiles] = NULL;
    
    freemb=get_partition_free("/sdcard")*1048576; // free Mb in byte
    LOGI("Free Mb on /sdcard: %i\n",freemb);
    for (i = 0 ; i < numFiles; i++)
    {
        list[i] = strdup(files[i]);
        if (stat(list[i], &filedata) < 0) {
           LOGE("Error stat'ing %s: %s\n", list[i], strerror(errno));
           return 1;
        }
        LOGI("Filesize: %i\n",filedata.st_size);       
        if(maxmb < filedata.st_size){
            maxmb = filedata.st_size;
            LOGI("New max mb: %i\n",maxmb);
        }
    }
    
    // check available space...
    if(maxmb >= freemb){
        ui_print("Not enough space on /sdcard, please\n");
        ui_print("free at least %i byte, exiting...\n",maxmb);
        return 1;        
    }    
        
    // let's go...    
    for (i = 0 ; i < numFiles; i++)
    {
        sprintf(cmd_zipalign,checkit,list[i]);
        fname = &list[i][strlen(directory)];
        if(0 == __system(cmd_zipalign)){
            //ui_print("Skipping %s\n",list[i]);
            zcount = zcount +1;
        }else{
            ui_print("Zipaligning %s...\n",fname);        
            sprintf(cmd_zipalign,doit,list[i],list[i]);
            if(0 == __system(cmd_zipalign)){
                //ui_print("Align: success\n",list[i]);
                sprintf(cmd_zipalign,copyit,list[i],list[i]);
                if(0 == __system(cmd_zipalign)){
                    //ui_print("Copyback: success %s\n",list[i]);
                    sprintf(cmd_zipalign,chmodit,list[i]);
                    if(0 == __system(cmd_zipalign)){
                        //ui_print("Chmod 644: sucess\n",list[i]);
                    }else{
                        ui_print("Chmod 644 failed...\n");                    
                    }
                }else{
                    ui_print("Copying back failed, skipping...\n");
                }
                sprintf(cmd_zipalign,removeit,list[i]);
                __system(cmd_zipalign);       
            }else{
                ui_print("Zipaligning failed, skipping...\n");
                sprintf(cmd_zipalign,removeit,list[i]);
                __system(cmd_zipalign);       
            }
        }
    }

    ui_print("Removing temp dir /sdcard/midnight_zipalign...\n");
    sprintf(tempdir,"rm -r /sdcard/midnight_zipalign"); 
    if(0 != __system(tempdir)){
        ui_print("Failed to to remove /sdcard/midnight_zipalign, exiting...\n",tempdir);
        return 1;
    }    
    ui_print("Already zipaligned, skipped: %i\n",zcount);
    ui_print("Done.\n");
    free_string_array(list);
    free_string_array(files);
    return 0;
}

void show_zipalign_menu() {
    static char* headers[] = {  "ZIPALIGN",
                                "Zipalign packages in chosen directory",
                                "for faster loading and execution time...",
                                NULL
    };

    static char* list[] = { "Zipalign packages in /system/app",
                            "Zipalign packages in /system/framework",
                            "Zipalign packages in /data/app",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
              if (confirm_selection("Confirm zipaligning in /system/app","Yes - zipalign in /system/app")) {
                apply_zipalign("/system/app/");
              }
              break;
            case 1:
              if (confirm_selection("Confirm zipaligning in /system/framework","Yes - zipalign in /system/framework")) {
                apply_zipalign("/system/framework/");
              }
              break;
            case 2:
              if (confirm_selection("Confirm zipaligning in /data/app","Yes - zipalign in /data/app")) {
                apply_zipalign("/data/app/");
              }
              break;
        }
    }

}

void show_options_menu() {
    const char* h[]={
        "misc. options",
        "",
        "",
        NULL};
    const char* m[]={
        "init.d/userinit.d            [default: off]",
        "1.128Ghz overclocking       [default: 1Ghz]",
        "touchwake                    [default: off]",
        "NOOP IO scheduler             [default:SIO]",
        "Ondemand CPU governor [default: Conservat.]",
        "512Kb sdcard readahead       [default: 256]",
        "USB fast_charge mode         [default: off]",
        NULL};
    int num=7;
    const char* cnfv[]={"INITD","OC1128","TOUCHWAKE","NOOP","ONDEMAND","512","FASTCHARGE"};
    const char* cnff="/data/local/midnight_options.conf";
    custom_menu(h,m,num,cnfv,cnff,0);
}

void show_uv_menu() {
    const char* h[]={
        "undervolting profiles",
        "",
        "",
        NULL};
    const char* m[]={
        " 0   0   0   0   0 mV [default]",
        " 0   0  25  50  75 mV",
        " 0   0  25  75 100 mV",
        " 0   0  50  75 125 mV",
        NULL};
    int num=4;
    const char* cnfv[]={"DEFAULT","UV1","UV2","UV3"};
    const char* cnff="/data/local/midnight_uv.conf";
    custom_menu(h,m,num,cnfv,cnff,1);
}

void show_vibration_menu() {
    const char* h[]={
        "vibration intensity profiles",
        "",
        "",
        NULL};
    const char* m[]={
        " none",
        " 25000",
        " 30000",
        " 35000",
        " stock [default]",
        NULL};
    int num=5;
    const char* cnfv[]={"VIB0","VIB1","VIB2","VIB3","DEFAULT"};
    const char* cnff="/data/local/midnight_vibration.conf";
    custom_menu(h,m,num,cnfv,cnff,1);
}

void show_led_menu() {
    const char* h[]={
        "touch LED timeout",
        "",
        "",
        NULL};
    const char* m[]={
        " 250ms",
        " 500ms",
        " 750ms",
        " 1000ms",
        " stock [default]",
        NULL};
    int num=5;
    const char* cnfv[]={"LED0","LED1","LED2","LED3","DEFAULT"};
    const char* cnff="/data/local/midnight_led.conf";
    custom_menu(h,m,num,cnfv,cnff,1);
}

void show_logcat_menu() {
    static char* headers[] = {  "Logcat module",
                                NULL
    };

    static char* list[] = { "enable Logcat module",
                            "disable Logcat module",
                            NULL
    };
    if (0 == file_exists("/data/local/logger.ko"))
        ui_print("\nLogger module is currently installed\n");
    else
        ui_print("\nLogger module is currently not installed\n");
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
                if(0 == __system("cp /system/lib/modules/logger.ko /data/local/logger.ko")){
                    ui_print("\nModule installed, please reboot.\n");
                }else{
                    ui_print("\nInstalling module failed, sorry.\n");
                }               
                break;
            case 1:                
                if(0 == __system("rm /data/local/logger.ko")){
                    ui_print("\nModule removed, please reboot.\n");
                }else{
                    ui_print("\nRemoving module failed, sorry.\n");
                }               
                break;
        }
    }
}

void show_midnight_menu() {
    static char* headers[] = {  "MNICS menu",
                                NULL
    };

    static char* list[] = { "options",
                            "undervolting profiles",
                            "vibration intensity",
                            "touch LED timeout",
                            "logcat",
                            "remove init.d content",
                            "remove nstools settings",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0: // options
                show_options_menu();
              break;
            case 1: // uv
                show_uv_menu();
              break;
            case 2: // uv
                show_vibration_menu();
              break;
            case 3: // uv
                show_led_menu();
              break;
            case 4: // logcat
                if (0 == ensure_path_mounted("/system")){
                    if (0 == ensure_path_mounted("/data")){
                        show_logcat_menu();
                    } else {
                        ui_print("\nError mounting /data, exiting...\n");
                        break;
                    }
                } else {
                    ui_print("\nError mounting /system, exiting...\n");
                }
                break;
            case 5: // init.d
                if (confirm_selection("Confirm clearing init.d?", "Yes - completely clear init.d")){
                    if (0 == ensure_path_mounted("/system")){
                        if(0 == __system("rm -r /system/etc/init.d/*")){
                            ui_print("\n/system/etc/init.d cleared.\n");
                        }else{
                            ui_print("\nClearing init.d failed, sorry.\n");
                        }
                        ensure_path_unmounted("/system");                
                    }else{
                        ui_print("\nError mounting /system, exiting...!\n");
                        break;
                    }
                }
              break;
            case 6: // nstools
                if (confirm_selection("Confirm deleting NSTools settings?", "Yes - delete NSTools settings")){
                    if (0 == ensure_path_mounted("/data")){
                        ensure_path_mounted("/datadata"); // /data/data possible symlink to /datadata
                        if(0 == __system("rm /data/data/mobi.cyann.nstools/shared_prefs/mobi.cyann.nstools_preferences.xml")){
                            ui_print("\nNSTools settings deleted.\n");
                        }else{
                            ui_print("\nDeleting NSTools settings failed, sorry.\n");
                        } 
                        ensure_path_unmounted("/datadata");
                        ensure_path_unmounted("/data");               
                    }else{
                        ui_print("\nError mounting /data, exiting...!\n");
                        break;
                    }
                }
              break;
        }
    }
}
