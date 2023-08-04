/*
 * Shairport, an Apple Airplay receiver
 * Copyright (c) James Laird 2013
 * All rights reserved.
 * Modifications and additions (c) Mike Brady 2014--2023
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libconfig.h>
#include <libgen.h>
#include <memory.h>
#include <net/if.h>
#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

#ifdef CONFIG_AIRPLAY_2
#include "ptp-utilities.h"
#include <gcrypt.h>
#include <libavcodec/avcodec.h>
#include <sodium.h>
#include <uuid/uuid.h>
#endif

#ifdef CONFIG_MBEDTLS
#include <mbedtls/md5.h>
#include <mbedtls/version.h>
#endif

#ifdef CONFIG_POLARSSL
#include <polarssl/md5.h>
#endif

#ifdef CONFIG_OPENSSL
#include <openssl/md5.h>
#endif

#if defined(CONFIG_DBUS_INTERFACE)
#include <glib.h>
#endif

#include "activity_monitor.h"
#include "audio.h"
#include "common.h"
#include "rtp.h"
#include "rtsp.h"

#if defined(CONFIG_DACP_CLIENT)
#include "dacp.h"
#endif

#if defined(CONFIG_METADATA_HUB)
#include "metadata_hub.h"
#endif

#ifdef CONFIG_DBUS_INTERFACE
#include "dbus-service.h"
#endif

#ifdef CONFIG_MQTT
#include "mqtt.h"
#endif

#ifdef CONFIG_MPRIS_INTERFACE
#include "mpris-service.h"
#endif

#ifdef CONFIG_LIBDAEMON
#include <libdaemon/dexec.h>
#include <libdaemon/dfork.h>
#include <libdaemon/dlog.h>
#include <libdaemon/dpid.h>
#include <libdaemon/dsignal.h>
#else
#include <syslog.h>
#endif

#ifdef CONFIG_SOXR
#include <math.h>
#include <soxr.h>
#endif

#ifdef CONFIG_CONVOLUTION
#include <FFTConvolver/convolver.h>
#endif

pid_t pid;
#ifdef CONFIG_LIBDAEMON
int this_is_the_daemon_process = 0;
#endif

#ifndef UUID_STR_LEN
#define UUID_STR_LEN 36
#endif

#define strnull(s) ((s) ? (s) : "(null)")

pthread_t rtsp_listener_thread;

int killOption = 0;
int daemonisewith = 0;
int daemonisewithout = 0;
int log_to_syslog_selected = 0;
#ifdef CONFIG_LIBDAEMON
int log_to_default = 1; // needed if libdaemon used
#endif
int display_config_selected = 0;
int log_to_syslog_select_is_first_command_line_argument = 0;

char configuration_file_path[4096 + 1];
char *config_file_real_path = NULL;

char first_backend_name[256];

void print_version(void) {
  char *version_string = get_version_string();
  if (version_string) {
    printf("%s\n", version_string);
    free(version_string);
  } else {
    debug(1, "Can't print version string!");
  }
}

#ifdef CONFIG_AIRPLAY_2
int has_fltp_capable_aac_decoder(void) {

  // return 1 if the AAC decoder advertises fltp decoding capability, which
  // is needed for decoding Buffered Audio streams
  int has_capability = 0;
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (codec != NULL) {
    const enum AVSampleFormat *p = codec->sample_fmts;
    if (p != NULL) {
      while ((has_capability == 0) && (*p != AV_SAMPLE_FMT_NONE)) {
        if (*p == AV_SAMPLE_FMT_FLTP)
          has_capability = 1;
        p++;
      }
    }
  }
  return has_capability;
}
#endif

#ifdef CONFIG_SOXR
pthread_t soxr_time_check_thread;
int soxr_time_check_thread_started = 0;
void *soxr_time_check(__attribute__((unused)) void *arg) {
  const int buffer_length = 352;
  int32_t inbuffer[buffer_length * 2];
  int32_t outbuffer[(buffer_length + 1) * 2];

  // int32_t *outbuffer = (int32_t*)malloc((buffer_length+1)*2*sizeof(int32_t));
  // int32_t *inbuffer = (int32_t*)malloc((buffer_length)*2*sizeof(int32_t));

  // generate a sample signal
  const double frequency = 440; //

  int i;

  int number_of_iterations = 0;
  uint64_t soxr_start_time = get_absolute_time_in_ns();
  uint64_t loop_until_time =
      (uint64_t)1500000000 + soxr_start_time; // loop for a second and a half, max -- no need to be
                                              // able to cancel it, do _don't even try_!
  while (get_absolute_time_in_ns() < loop_until_time) {

    number_of_iterations++;
    for (i = 0; i < buffer_length; i++) {
      double w = sin(i * (frequency + number_of_iterations * 2) * 2 * M_PI / 44100);
      int32_t wint = (int32_t)(w * INT32_MAX);
      inbuffer[i * 2] = wint;
      inbuffer[i * 2 + 1] = wint;
    }

    soxr_io_spec_t io_spec;
    io_spec.itype = SOXR_INT32_I;
    io_spec.otype = SOXR_INT32_I;
    io_spec.scale = 1.0; // this seems to crash if not = 1.0
    io_spec.e = NULL;
    io_spec.flags = 0;

    size_t odone;

    soxr_oneshot(buffer_length, buffer_length + 1, 2,  // Rates and # of chans.
                 inbuffer, buffer_length, NULL,        // Input.
                 outbuffer, buffer_length + 1, &odone, // Output.
                 &io_spec,                             // Input, output and transfer spec.
                 NULL, NULL);                          // Default configuration.

    io_spec.itype = SOXR_INT32_I;
    io_spec.otype = SOXR_INT32_I;
    io_spec.scale = 1.0; // this seems to crash if not = 1.0
    io_spec.e = NULL;
    io_spec.flags = 0;

    soxr_oneshot(buffer_length, buffer_length - 1, 2,  // Rates and # of chans.
                 inbuffer, buffer_length, NULL,        // Input.
                 outbuffer, buffer_length - 1, &odone, // Output.
                 &io_spec,                             // Input, output and transfer spec.
                 NULL, NULL);                          // Default configuration.
  }

  int64_t soxr_execution_time =
      get_absolute_time_in_ns() - soxr_start_time;   // this must be zero or positive
  int soxr_execution_time_int = soxr_execution_time; // must be in or around 1500000000

  // free(outbuffer);
  // free(inbuffer);

  if (number_of_iterations != 0) {
    config.soxr_delay_index = soxr_execution_time_int / number_of_iterations;
  } else {
    debug(1, "No soxr-timing iterations performed, so \"basic\" iteration will be used.");
    config.soxr_delay_index = 0; // used as a flag
  }
  debug(2, "soxr_delay: %d nanoseconds, soxr_delay_threshold: %d milliseconds.",
        config.soxr_delay_index, config.soxr_delay_threshold / 1000000);
  if ((config.packet_stuffing == ST_soxr) &&
      (config.soxr_delay_index > config.soxr_delay_threshold))
    inform("Note: this device may be too slow for \"soxr\" interpolation. Consider choosing the "
           "\"basic\" or \"auto\" interpolation setting.");
  if (config.packet_stuffing == ST_auto)
    debug(
        1, "\"%s\" interpolation has been chosen.",
        ((config.soxr_delay_index != 0) && (config.soxr_delay_index <= config.soxr_delay_threshold))
            ? "soxr"
            : "basic");
  pthread_exit(NULL);
}

#endif

void usage(char *progname) {

#ifdef CONFIG_AIRPLAY_2
  if (has_fltp_capable_aac_decoder() == 0) {
    printf("\nIMPORTANT NOTE: Shairport Sync can not run on this system.\n");
    printf("A Floating Planar (\"fltp\") AAC decoder is required, ");
    printf("but the system's ffmpeg library does not seem to include one.\n");
    printf("See: "
           "https://github.com/mikebrady/shairport-sync/blob/development/"
           "TROUBLESHOOTING.md#aac-decoder-issues-airplay-2-only\n\n");

  } else {
#endif
    // clang-format off
    printf("Please use the configuration file for settings where possible.\n");
    printf("Many more settings are available in the configuration file.\n");
    printf("\n");
    printf("Usage: %s [options...]\n", progname);
    printf("  or:  %s [options...] -- [audio output-specific options]\n", progname);
    printf("\n");
    printf("Options:\n");
    printf("    -h, --help              Show this help.\n");
    printf("    -V, --version           Show version information -- the version string.\n");
    printf("    -X, --displayConfig     Output OS information, version string, command line, configuration file and active settings to the log.\n");
    printf("    --statistics            Print some interesting statistics. More will be printed if -v / -vv / -vvv are also chosen.\n");
    printf("    -v, --verbose           Print debug information; -v some; -vv more; -vvv lots -- generally too much.\n");
    printf("    -c, --configfile=FILE   Read configuration settings from FILE. Default is %s.\n", configuration_file_path);
    printf("    -a, --name=NAME         Set service name. Default is the hostname with first letter capitalised.\n");
    printf("    --password=PASSWORD     Require PASSWORD to connect. Default is no password. (Classic AirPlay only.)\n");
    printf("    -p, --port=PORT         Set RTSP listening port. Default 5000; 7000 for AirPlay 2./\n");
    printf("    -L, --latency=FRAMES    [Deprecated] Set the latency for audio sent from an unknown device.\n");
    printf("                            The default is to set it automatically.\n");
    printf("    -S, --stuffing=MODE     Set how to adjust current latency to match desired latency, where:\n");
    printf("                            \"basic\" inserts or deletes audio frames from packet frames with low processor overhead, and\n");
    printf("                            \"soxr\" uses libsoxr to minimally resample packet frames -- moderate processor overhead.\n");
    printf("                            The default \"auto\" setting chooses basic or soxr depending on processor capability.\n");
    printf("                            The \"soxr\" option is only available if built with soxr support.\n");
    printf("    -B, --on-start=PROGRAM  Run PROGRAM when playback is about to begin.\n");
    printf("    -E, --on-stop=PROGRAM   Run PROGRAM when playback has ended.\n");
    printf("                            For -B and -E options, specify the full path to the program and arguments, e.g. \"/usr/bin/logger\".\n");
    printf("                            Executable scripts work, but the file must be marked executable have the appropriate shebang (#!/bin/sh) on the first line.\n");
    printf("    -w, --wait-cmd          Wait until the -B or -E programs finish before continuing.\n");
    printf("    -o, --output=BACKEND    Select audio backend. They are listed at the end of this text. The first one is the default.\n");
    printf("    -m, --mdns=BACKEND      Use the mDNS backend named BACKEND to advertise the AirPlay service through Bonjour/ZeroConf.\n");
    printf("                            They are listed at the end of this text.\n");
    printf("                            If no mdns backend is specified, they are tried in order until one works.\n");
    printf("    -r, --resync=THRESHOLD  [Deprecated] resync if error exceeds this number of frames. Set to 0 to stop resyncing.\n");
    printf("    -t, --timeout=SECONDS   Go back to idle mode from play mode after a break in communications of this many seconds (default 120). Set to 0 never to exit play mode.\n");
    printf("    --tolerance=TOLERANCE   [Deprecated] Allow a synchronization error of TOLERANCE frames (default 88) before trying to correct it.\n");
    printf("    --logOutputLevel        Log the output level setting -- a debugging option, useful for determining the optimum maximum volume.\n");
#ifdef CONFIG_LIBDAEMON
    printf("    -d, --daemon            Daemonise.\n");
    printf("    -j, --justDaemoniseNoPIDFile            Daemonise without a PID file.\n");
    printf("    -k, --kill              Kill the existing shairport daemon.\n");
#endif
#ifdef CONFIG_METADATA
    printf("    -M, --metadata-enable   Ask for metadata from the source and process it. Much more flexibility with configuration file settings.\n");
    printf("    --metadata-pipename=PIPE send metadata to PIPE, e.g. --metadata-pipename=/tmp/%s-metadata.\n", config.appName);
    printf("                            The default is /tmp/%s-metadata.\n", config.appName);
    printf("    -g, --get-coverart      Include cover art in the metadata to be gathered and sent.\n");
#endif
    printf("    --log-to-syslog         Send debug and statistics information through syslog\n");
    printf("                            If used, this should be the first command line argument.\n");
    printf("    -u, --use-stderr        [Deprecated] This setting is not needed -- stderr is now used by default and syslog is selected using --log-to-syslog.\n");
    printf("\n");
    mdns_ls_backends();
    printf("\n");
    audio_ls_outputs();
    // clang-format on

#ifdef CONFIG_AIRPLAY_2
  }
#endif
}

int parse_options(int argc, char **argv) {
  // there are potential memory leaks here -- it's called a second time, previously allocated
  // strings will dangle.
  char *raw_service_name = NULL; /* Used to pick up the service name before possibly expanding it */
  char *stuffing = NULL;         /* used for picking up the stuffing option */
  signed char c;                 /* used for argument parsing */
  // int i = 0;                     /* used for tracking options */
  int resync_threshold_in_frames = 0;
  int tolerance_in_frames = 0;
  poptContext optCon; /* context for parsing command-line options */
  struct poptOption optionsTable[] = {
      {"verbose", 'v', POPT_ARG_NONE, NULL, 'v', NULL, NULL},
      {"kill", 'k', POPT_ARG_NONE, &killOption, 0, NULL, NULL},
      {"daemon", 'd', POPT_ARG_NONE, &daemonisewith, 0, NULL, NULL},
      {"justDaemoniseNoPIDFile", 'j', POPT_ARG_NONE, &daemonisewithout, 0, NULL, NULL},
      {"configfile", 'c', POPT_ARG_STRING, &config.configfile, 0, NULL, NULL},
      {"statistics", 0, POPT_ARG_NONE, &config.statistics_requested, 0, NULL, NULL},
      {"logOutputLevel", 0, POPT_ARG_NONE, &config.logOutputLevel, 0, NULL, NULL},
      {"version", 'V', POPT_ARG_NONE, NULL, 0, NULL, NULL},
      {"displayConfig", 'X', POPT_ARG_NONE, &display_config_selected, 0, NULL, NULL},
      {"port", 'p', POPT_ARG_INT, &config.port, 0, NULL, NULL},
      {"name", 'a', POPT_ARG_STRING, &raw_service_name, 0, NULL, NULL},
      {"output", 'o', POPT_ARG_STRING, &config.output_name, 0, NULL, NULL},
      {"on-start", 'B', POPT_ARG_STRING, &config.cmd_start, 0, NULL, NULL},
      {"on-stop", 'E', POPT_ARG_STRING, &config.cmd_stop, 0, NULL, NULL},
      {"wait-cmd", 'w', POPT_ARG_NONE, &config.cmd_blocking, 0, NULL, NULL},
      {"mdns", 'm', POPT_ARG_STRING, &config.mdns_name, 0, NULL, NULL},
      {"latency", 'L', POPT_ARG_INT, &config.userSuppliedLatency, 0, NULL, NULL},
      {"stuffing", 'S', POPT_ARG_STRING, &stuffing, 'S', NULL, NULL},
      {"resync", 'r', POPT_ARG_INT, &resync_threshold_in_frames, 'r', NULL, NULL},
      {"timeout", 't', POPT_ARG_INT, &config.timeout, 't', NULL, NULL},
      {"password", 0, POPT_ARG_STRING, &config.password, 0, NULL, NULL},
      {"tolerance", 'z', POPT_ARG_INT, &tolerance_in_frames, 'z', NULL, NULL},
      {"use-stderr", 'u', POPT_ARG_NONE, NULL, 'u', NULL, NULL},
      {"log-to-syslog", 0, POPT_ARG_NONE, &log_to_syslog_selected, 0, NULL, NULL},
#ifdef CONFIG_METADATA
      {"metadata-enable", 'M', POPT_ARG_NONE, &config.metadata_enabled, 'M', NULL, NULL},
      {"metadata-pipename", 0, POPT_ARG_STRING, &config.metadata_pipename, 0, NULL, NULL},
      {"get-coverart", 'g', POPT_ARG_NONE, &config.get_coverart, 'g', NULL, NULL},
#endif
      POPT_AUTOHELP{NULL, 0, 0, NULL, 0, NULL, NULL}};

  // we have to parse the command line arguments to look for a config file
  int optind;
  optind = argc;
  int j;
  for (j = 0; j < argc; j++)
    if (strcmp(argv[j], "--") == 0)
      optind = j;

  optCon = poptGetContext(NULL, optind, (const char **)argv, optionsTable, 0);
  if (optCon == NULL)
    die("Can not get a secondary popt context.");
  poptSetOtherOptionHelp(optCon, "[OPTIONS]* ");

  /* Now do options processing just to get a debug log destination and level */
  debuglev = 0;
  while ((c = poptGetNextOpt(optCon)) >= 0) {
    switch (c) {
    case 'v':
      debuglev++;
      break;
    case 'u':
      inform("Warning: the option -u is no longer needed and is deprecated. Debug and statistics "
             "output to STDERR is now the default. Use \"--log-to-syslog\" to revert.");
      break;
    case 'D':
      inform("Warning: the option -D or --disconnectFromOutput is deprecated.");
      break;
    case 'R':
      inform("Warning: the option -R or --reconnectToOutput is deprecated.");
      break;
    case 'A':
      inform("Warning: the option -A or --AirPlayLatency is deprecated and ignored. This setting "
             "is now "
             "automatically received from the AirPlay device.");
      break;
    case 'i':
      inform("Warning: the option -i or --iTunesLatency is deprecated and ignored. This setting is "
             "now "
             "automatically received from iTunes");
      break;
    case 'f':
      inform(
          "Warning: the option --forkedDaapdLatency is deprecated and ignored. This setting is now "
          "automatically received from forkedDaapd");
      break;
    case 'r':
      config.resync_threshold = (resync_threshold_in_frames * 1.0) / 44100;
      inform("Warning: the option -r or --resync is deprecated. Please use the "
             "\"resync_threshold_in_seconds\" setting in the config file instead.");
      break;
    case 'z':
      config.tolerance = (tolerance_in_frames * 1.0) / 44100;
      inform("Warning: the option --tolerance is deprecated. Please use the "
             "\"drift_tolerance_in_seconds\" setting in the config file instead.");
      break;
    }
  }
  if (c < -1) {
    die("%s: %s", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
  }

  poptFreeContext(optCon);

  if (log_to_syslog_selected) {
    // if this was the first command line argument, it'll already have been chosen
    if (log_to_syslog_select_is_first_command_line_argument == 0) {
      inform("Suggestion: make \"--log-to-syslog\" the first command line argument to ensure "
             "messages go to the syslog right from the beginning.");
    }
#ifdef CONFIG_LIBDAEMON
    log_to_default = 0; // a specific log output modality has been selected.
#endif
    log_to_syslog();
  }

#ifdef CONFIG_LIBDAEMON
  if ((daemonisewith) && (daemonisewithout))
    die("Select either daemonize_with_pid_file or daemonize_without_pid_file -- you have selected "
        "both!");
  if ((daemonisewith) || (daemonisewithout)) {
    config.daemonise = 1;
    if (daemonisewith)
      config.daemonise_store_pid = 1;
  };
#endif

  config.audio_backend_silent_lead_in_time_auto =
      1; // start outputting silence as soon as packets start arriving
  config.default_airplay_volume = -24.0;
  config.high_threshold_airplay_volume =
      -16.0; // if the volume exceeds this, reset to the default volume if idle for the
             // limit_to_high_volume_threshold_time_in_minutes time
  config.limit_to_high_volume_threshold_time_in_minutes =
      0; // after this time in minutes, if the volume is higher, use the default_airplay_volume
         // volume for new play sessions.
  config.fixedLatencyOffset = 11025; // this sounds like it works properly.
  config.diagnostic_drop_packet_fraction = 0.0;
  config.active_state_timeout = 10.0;
  config.soxr_delay_threshold = 30 * 1000000; // the soxr measurement time (nanoseconds) of two
                                              // oneshots must not exceed this if soxr interpolation
                                              // is to be chosen automatically.
  config.volume_range_hw_priority =
      0; // if combining software and hardware volume control, give the software priority
         // i.e. when reducing volume, reduce the sw first before reducing the software.
         // this is because some hw mixers mute at the bottom of their range, and they don't always
  // advertise this fact
  config.resend_control_first_check_time =
      0.10; // wait this many seconds before requesting the resending of a missing packet
  config.resend_control_check_interval_time =
      0.25; // wait this many seconds before again requesting the resending of a missing packet
  config.resend_control_last_check_time =
      0.10; // give up if the packet is still missing this close to when it's needed
  config.missing_port_dacp_scan_interval_seconds =
      2.0; // check at this interval if no DACP port number is known

  config.minimum_free_buffer_headroom = 125; // leave approximately one second's worth of buffers
                                             // free after calculating the effective latency.
  // e.g. if we have 1024 buffers or 352 frames = 8.17 seconds and we have a nominal latency of 2.0
  // seconds then we can add an offset of 5.17 seconds and still leave a second's worth of buffers
  // for unexpected circumstances

#ifdef CONFIG_METADATA
  /* Get the metadata setting. */
  config.metadata_enabled = 1; // if metadata support is included, then enable it by default
  config.get_coverart = 1;     // if metadata support is included, then enable it by default
#endif

#ifdef CONFIG_CONVOLUTION
  config.convolution_max_length = 8192;
#endif
  config.loudness_reference_volume_db = -20;

#ifdef CONFIG_METADATA_HUB
  config.cover_art_cache_dir = "/tmp/shairport-sync/.cache/coverart";
  config.scan_interval_when_active =
      1; // number of seconds between DACP server scans when playing something
  config.scan_interval_when_inactive =
      1; // number of seconds between DACP server scans when playing nothing
  config.scan_max_bad_response_count =
      5; // number of successive bad results to ignore before giving up
  // config.scan_max_inactive_count =
  //    (365 * 24 * 60 * 60) / config.scan_interval_when_inactive; // number of scans to do before
  //    stopping if
  // not made active again (not used)
#endif

#ifdef CONFIG_AIRPLAY_2
  // the features code is a 64-bit number, but in the mDNS advertisement, the least significant 32
  // bit are given first for example, if the features number is 0x1C340405F4A00, it will be given as
  // features=0x405F4A00,0x1C340 in the mDNS string, and in a signed decimal number in the plist:
  // 496155702020608 this setting here is the source of both the plist features response and the
  // mDNS string.
  // note: 0x300401F4A00 works but with weird delays and stuff
  // config.airplay_features = 0x1C340405FCA00;
  uint64_t mask =
      ((uint64_t)1 << 17) | ((uint64_t)1 << 16) | ((uint64_t)1 << 15) | ((uint64_t)1 << 50);
  config.airplay_features =
      0x1C340405D4A00 & (~mask); // APX + Authentication4 (b14) with no metadata (see below)
  // Advertised with mDNS and returned with GET /info, see
  // https://openairplay.github.io/airplay-spec/status_flags.html 0x4: Audio cable attached, no PIN
  // required (transient pairing), 0x204: Audio cable attached, OneTimePairingRequired 0x604: Audio
  // cable attached, OneTimePairingRequired, device was setup for Homekit access control
  config.airplay_statusflags = 0x04;
  // Set to NULL to work with transient pairing
  config.airplay_pin = NULL;

  // use the MAC address placed in config.hw_addr to generate the default airplay_device_id
  uint64_t temporary_airplay_id = nctoh64(config.hw_addr);
  temporary_airplay_id =
      temporary_airplay_id >> 16; // we only use the first 6 bytes but have imported 8.

  // now generate a UUID
  // from https://stackoverflow.com/questions/51053568/generating-a-random-uuid-in-c
  // with thanks
  uuid_t binuuid;
  uuid_generate_random(binuuid);

  char *uuid = malloc(UUID_STR_LEN + 1); // leave space for the NUL at the end
  // Produces a UUID string at uuid consisting of lower-case letters
  uuid_unparse_lower(binuuid, uuid);
  config.airplay_pi = uuid;

#endif

  // config_setting_t *setting;
  const char *str = 0;
  int value = 0;
  double dvalue = 0.0;

  // debug(1, "Looking for the configuration file \"%s\".", config.configfile);

  config_init(&config_file_stuff);

  config_file_real_path = realpath(config.configfile, NULL);
  if (config_file_real_path == NULL) {
    debug(2, "can't resolve the configuration file \"%s\".", config.configfile);
  } else {
    debug(2, "looking for configuration file at full path \"%s\"", config_file_real_path);
    /* Read the file. If there is an error, report it and exit. */
    if (config_read_file(&config_file_stuff, config_file_real_path)) {
      config_set_auto_convert(&config_file_stuff,
                              1); // allow autoconversion from int/float to int/float
      // make config.cfg point to it
      config.cfg = &config_file_stuff;

      /* Get the Service Name. */
      if (config_lookup_string(config.cfg, "general.name", &str)) {
        raw_service_name = (char *)str;
      }
#ifdef CONFIG_LIBDAEMON
      /* Get the Daemonize setting. */
      config_set_lookup_bool(config.cfg, "sessioncontrol.daemonize_with_pid_file", &daemonisewith);

      /* Get the Just_Daemonize setting. */
      config_set_lookup_bool(config.cfg, "sessioncontrol.daemonize_without_pid_file",
                             &daemonisewithout);

      /* Get the directory path for the pid file created when the program is daemonised. */
      if (config_lookup_string(config.cfg, "sessioncontrol.daemon_pid_dir", &str))
        config.piddir = (char *)str;
#endif

      /* Get the mdns_backend setting. */
      if (config_lookup_string(config.cfg, "general.mdns_backend", &str))
        config.mdns_name = (char *)str;

      /* Get the output_backend setting. */
      if (config_lookup_string(config.cfg, "general.output_backend", &str))
        config.output_name = (char *)str;

      /* Get the port setting. */
      if (config_lookup_int(config.cfg, "general.port", &value)) {
        if ((value < 0) || (value > 65535))
#ifdef CONFIG_AIRPLAY_2
          die("Invalid port number  \"%sd\". It should be between 0 and 65535, default is 7000",
              value);
#else
          die("Invalid port number  \"%sd\". It should be between 0 and 65535, default is 5000",
              value);
#endif
        else
          config.port = value;
      }

      /* Get the udp port base setting. */
      if (config_lookup_int(config.cfg, "general.udp_port_base", &value)) {
        if ((value < 0) || (value > 65535))
          die("Invalid port number  \"%sd\". It should be between 0 and 65535, default is 6001",
              value);
        else
          config.udp_port_base = value;
      }

      /* Get the udp port range setting. This is number of ports that will be tried for free ports ,
       * starting at the port base. Only three ports are needed. */
      if (config_lookup_int(config.cfg, "general.udp_port_range", &value)) {
        if ((value < 3) || (value > 65535))
          die("Invalid port range  \"%sd\". It should be between 3 and 65535, default is 10",
              value);
        else
          config.udp_port_range = value;
      }

      /* Get the password setting. */
      if (config_lookup_string(config.cfg, "general.password", &str))
        config.password = (char *)str;

      if (config_lookup_string(config.cfg, "general.interpolation", &str)) {
        if (strcasecmp(str, "basic") == 0)
          config.packet_stuffing = ST_basic;
        else if (strcasecmp(str, "auto") == 0)
          config.packet_stuffing = ST_auto;
        else if (strcasecmp(str, "soxr") == 0)
#ifdef CONFIG_SOXR
          config.packet_stuffing = ST_soxr;
#else
          warn("The soxr option not available because this version of shairport-sync was built "
               "without libsoxr "
               "support. Change the \"general/interpolation\" setting in the configuration file.");
#endif
        else
          die("Invalid interpolation option choice \"%s\". It should be \"auto\", \"basic\" or "
              "\"soxr\"",
              str);
      }

#ifdef CONFIG_SOXR

      /* Get the soxr_delay_threshold setting. */
      /* Convert between the input, given in milliseconds, and the stored values in nanoseconds. */
      if (config_lookup_int(config.cfg, "general.soxr_delay_threshold", &value)) {
        if ((value >= 1) && (value <= 100))
          config.soxr_delay_threshold = value * 1000000;
        else
          warn("Invalid general soxr_delay_threshold setting option choice \"%d\". It should be "
               "between 1 and 100, "
               "inclusive. Default is %d (milliseconds).",
               value, config.soxr_delay_threshold / 1000000);
      }
#endif

      /* Get the statistics setting. */
      if (config_set_lookup_bool(config.cfg, "general.statistics",
                                 &(config.statistics_requested))) {
        warn("The \"general\" \"statistics\" setting is deprecated. Please use the \"diagnostics\" "
             "\"statistics\" setting instead.");
      }

      /* The old drift tolerance setting. */
      if (config_lookup_int(config.cfg, "general.drift", &value)) {
        inform("The drift setting is deprecated. Use "
               "drift_tolerance_in_seconds instead");
        config.tolerance = 1.0 * value / 44100;
      }

      /* The old resync setting. */
      if (config_lookup_int(config.cfg, "general.resync_threshold", &value)) {
        inform("The resync_threshold setting is deprecated. Use "
               "resync_threshold_in_seconds instead");
        config.resync_threshold = 1.0 * value / 44100;
      }

      /* Get the drift tolerance setting. */
      if (config_lookup_float(config.cfg, "general.drift_tolerance_in_seconds", &dvalue))
        config.tolerance = dvalue;

      /* Get the resync setting. */
      if (config_lookup_float(config.cfg, "general.resync_threshold_in_seconds", &dvalue))
        config.resync_threshold = dvalue;

      /* Get the resync recovery time setting. */
      if (config_lookup_float(config.cfg, "general.resync_recovery_time_in_seconds", &dvalue))
        config.resync_recovery_time = dvalue;

      /* Get the verbosity setting. */
      if (config_lookup_int(config.cfg, "general.log_verbosity", &value)) {
        warn("The \"general\" \"log_verbosity\" setting is deprecated. Please use the "
             "\"diagnostics\" \"log_verbosity\" setting instead.");
        if ((value >= 0) && (value <= 3))
          debuglev = value;
        else
          die("Invalid log verbosity setting option choice \"%d\". It should be between 0 and 3, "
              "inclusive.",
              value);
      }

      /* Get the verbosity setting. */
      if (config_lookup_int(config.cfg, "diagnostics.log_verbosity", &value)) {
        if ((value >= 0) && (value <= 3))
          debuglev = value;
        else
          die("Invalid diagnostics log_verbosity setting option choice \"%d\". It should be "
              "between 0 and 3, "
              "inclusive.",
              value);
      }

      /* Get the config.debugger_show_file_and_line in debug messages setting. */
      if (config_lookup_string(config.cfg, "diagnostics.log_show_file_and_line", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.debugger_show_file_and_line = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.debugger_show_file_and_line = 1;
        else
          die("Invalid diagnostics log_show_file_and_line option choice \"%s\". It should be "
              "\"yes\" or \"no\"",
              str);
      }

      /* Get the show elapsed time in debug messages setting. */
      if (config_lookup_string(config.cfg, "diagnostics.log_show_time_since_startup", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.debugger_show_elapsed_time = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.debugger_show_elapsed_time = 1;
        else
          die("Invalid diagnostics log_show_time_since_startup option choice \"%s\". It should be "
              "\"yes\" or \"no\"",
              str);
      }

      /* Get the show relative time in debug messages setting. */
      if (config_lookup_string(config.cfg, "diagnostics.log_show_time_since_last_message", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.debugger_show_relative_time = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.debugger_show_relative_time = 1;
        else
          die("Invalid diagnostics log_show_time_since_last_message option choice \"%s\". It "
              "should be \"yes\" or \"no\"",
              str);
      }

      /* Get the statistics setting. */
      if (config_lookup_string(config.cfg, "diagnostics.statistics", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.statistics_requested = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.statistics_requested = 1;
        else
          die("Invalid diagnostics statistics option choice \"%s\". It should be \"yes\" or "
              "\"no\"",
              str);
      }

      /* Get the disable_resend_requests setting. */
      if (config_lookup_string(config.cfg, "diagnostics.disable_resend_requests", &str)) {
        config.disable_resend_requests = 0; // this is for legacy -- only set by -t 0
        if (strcasecmp(str, "no") == 0)
          config.disable_resend_requests = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.disable_resend_requests = 1;
        else
          die("Invalid diagnostic disable_resend_requests option choice \"%s\". It should be "
              "\"yes\" "
              "or \"no\"",
              str);
      }

      /* Get the drop packets setting. */
      if (config_lookup_float(config.cfg, "diagnostics.drop_this_fraction_of_audio_packets",
                              &dvalue)) {
        if ((dvalue >= 0.0) && (dvalue <= 3.0))
          config.diagnostic_drop_packet_fraction = dvalue;
        else
          die("Invalid diagnostics drop_this_fraction_of_audio_packets setting \"%d\". It should "
              "be "
              "between 0.0 and 1.0, "
              "inclusive.",
              dvalue);
      }

      /* Get the diagnostics output default. */
      if (config_lookup_string(config.cfg, "diagnostics.log_output_to", &str)) {
#ifdef CONFIG_LIBDAEMON
        log_to_default = 0; // a specific log output modality has been selected.
#endif
        if (strcasecmp(str, "syslog") == 0)
          log_to_syslog();
        else if (strcasecmp(str, "stdout") == 0) {
          log_to_stdout();
        } else if (strcasecmp(str, "stderr") == 0) {
          log_to_stderr();
        } else {
          config.log_file_path = (char *)str;
          config.log_fd = -1;
          log_to_file();
        }
      }
      /* Get the ignore_volume_control setting. */
      if (config_lookup_string(config.cfg, "general.ignore_volume_control", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.ignore_volume_control = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.ignore_volume_control = 1;
        else
          die("Invalid ignore_volume_control option choice \"%s\". It should be \"yes\" or \"no\"",
              str);
      }

      /* Get the optional volume_max_db setting. */
      if (config_lookup_float(config.cfg, "general.volume_max_db", &dvalue)) {
        // debug(1, "Max volume setting of %f dB", dvalue);
        config.volume_max_db = dvalue;
        config.volume_max_db_set = 1;
      }

      /* Get the optional default_volume setting. */
      if (config_lookup_float(config.cfg, "general.default_airplay_volume", &dvalue)) {
        // debug(1, "Default airplay volume setting of %f on the -30.0 to 0 scale", dvalue);
        if ((dvalue >= -30.0) && (dvalue <= 0.0)) {
          config.default_airplay_volume = dvalue;
        } else {
          warn("The default airplay volume setting must be between -30.0 and 0.0.");
        }
      }

      /* Get the optional high_volume_threshold setting. */
      if (config_lookup_float(config.cfg, "general.high_threshold_airplay_volume", &dvalue)) {
        // debug(1, "High threshold airplay volume setting of %f on the -30.0 to 0 scale", dvalue);
        if ((dvalue >= -30.0) && (dvalue <= 0.0)) {
          config.high_threshold_airplay_volume = dvalue;
        } else {
          warn("The high threshold airplay volume setting must be between -30.0 and 0.0.");
        }
      }

      /* Get the optional high volume idle tiomeout setting. */
      if (config_lookup_float(config.cfg, "general.high_volume_idle_timeout_in_minutes", &dvalue)) {
        // debug(1, "High high_volume_idle_timeout_in_minutes setting of %f", dvalue);
        if (dvalue >= 0.0) {
          config.limit_to_high_volume_threshold_time_in_minutes = dvalue;
        } else {
          warn("The high volume idle timeout in minutes setting must be 0.0 or greater. A setting "
               "of 0.0 disables the high volume check.");
        }
      }

      if (config_lookup_string(config.cfg, "general.run_this_when_volume_is_set", &str)) {
        config.cmd_set_volume = (char *)str;
      }

      /* Get the playback_mode setting */
      if (config_lookup_string(config.cfg, "general.playback_mode", &str)) {
        if (strcasecmp(str, "stereo") == 0)
          config.playback_mode = ST_stereo;
        else if (strcasecmp(str, "mono") == 0)
          config.playback_mode = ST_mono;
        else if (strcasecmp(str, "reverse stereo") == 0)
          config.playback_mode = ST_reverse_stereo;
        else if (strcasecmp(str, "both left") == 0)
          config.playback_mode = ST_left_only;
        else if (strcasecmp(str, "both right") == 0)
          config.playback_mode = ST_right_only;
        else
          die("Invalid playback_mode choice \"%s\". It should be \"stereo\" (default), \"mono\", "
              "\"reverse stereo\", \"both left\", \"both right\"",
              str);
      }

      /* Get the volume control profile setting -- "standard" or "flat" */
      if (config_lookup_string(config.cfg, "general.volume_control_profile", &str)) {
        if (strcasecmp(str, "standard") == 0)
          config.volume_control_profile = VCP_standard;
        else if (strcasecmp(str, "flat") == 0)
          config.volume_control_profile = VCP_flat;
        else
          die("Invalid volume_control_profile choice \"%s\". It should be \"standard\" (default) "
              "or \"flat\"",
              str);
      }

      config_set_lookup_bool(config.cfg, "general.volume_control_combined_hardware_priority",
                             &config.volume_range_hw_priority);

      /* Get the interface to listen on, if specified Default is all interfaces */
      /* we keep the interface name and the index */

      if (config_lookup_string(config.cfg, "general.interface", &str))
        config.interface = strdup(str);

      if (config_lookup_string(config.cfg, "general.interface", &str)) {

        config.interface_index = if_nametoindex(config.interface);

        if (config.interface_index == 0) {
          inform(
              "The mdns service interface \"%s\" was not found, so the setting has been ignored.",
              config.interface);
          free(config.interface);
          config.interface = NULL;
        }
      }

      /* Get the regtype -- the service type and protocol, separated by a dot. Default is
       * "_raop._tcp" */
      if (config_lookup_string(config.cfg, "general.regtype", &str))
        config.regtype = strdup(str);

      /* Get the volume range, in dB, that should be used If not set, it means you just use the
       * range set by the mixer. */
      if (config_lookup_int(config.cfg, "general.volume_range_db", &value)) {
        if ((value < 30) || (value > 150))
          die("Invalid volume range  %d dB. It should be between 30 and 150 dB. Zero means use "
              "the mixer's native range. The setting reamins at %d.",
              value, config.volume_range_db);
        else
          config.volume_range_db = value;
      }

      /* Get the alac_decoder setting. */
      if (config_lookup_string(config.cfg, "general.alac_decoder", &str)) {
        if (strcasecmp(str, "hammerton") == 0)
          config.use_apple_decoder = 0;
        else if (strcasecmp(str, "apple") == 0) {
          if ((config.decoders_supported & 1 << decoder_apple_alac) != 0)
            config.use_apple_decoder = 1;
          else
            inform("Support for the Apple ALAC decoder has not been compiled into this version of "
                   "Shairport Sync. The default decoder will be used.");
        } else
          die("Invalid alac_decoder option choice \"%s\". It should be \"hammerton\" or \"apple\"",
              str);
      }

      /* Get the resend control settings. */
      if (config_lookup_float(config.cfg, "general.resend_control_first_check_time", &dvalue)) {
        if ((dvalue >= 0.0) && (dvalue <= 3.0))
          config.resend_control_first_check_time = dvalue;
        else
          warn("Invalid general resend_control_first_check_time setting \"%f\". It should "
               "be "
               "between 0.0 and 3.0, "
               "inclusive. The setting remains at %f seconds.",
               dvalue, config.resend_control_first_check_time);
      }

      if (config_lookup_float(config.cfg, "general.resend_control_check_interval_time", &dvalue)) {
        if ((dvalue >= 0.0) && (dvalue <= 3.0))
          config.resend_control_check_interval_time = dvalue;
        else
          warn("Invalid general resend_control_check_interval_time setting \"%f\". It should "
               "be "
               "between 0.0 and 3.0, "
               "inclusive. The setting remains at %f seconds.",
               dvalue, config.resend_control_check_interval_time);
      }

      if (config_lookup_float(config.cfg, "general.resend_control_last_check_time", &dvalue)) {
        if ((dvalue >= 0.0) && (dvalue <= 3.0))
          config.resend_control_last_check_time = dvalue;
        else
          warn("Invalid general resend_control_last_check_time setting \"%f\". It should "
               "be "
               "between 0.0 and 3.0, "
               "inclusive. The setting remains at %f seconds.",
               dvalue, config.resend_control_last_check_time);
      }

      if (config_lookup_float(config.cfg, "general.missing_port_dacp_scan_interval_seconds",
                              &dvalue)) {
        if ((dvalue >= 0.0) && (dvalue <= 300.0))
          config.missing_port_dacp_scan_interval_seconds = dvalue;
        else
          warn("Invalid general missing_port_dacp_scan_interval_seconds setting \"%f\". It should "
               "be "
               "between 0.0 and 300.0, "
               "inclusive. The setting remains at %f seconds.",
               dvalue, config.missing_port_dacp_scan_interval_seconds);
      }

      /* Get the default latency. Deprecated! */
      if (config_lookup_int(config.cfg, "latencies.default", &value))
        config.userSuppliedLatency = value;

#ifdef CONFIG_METADATA
      /* Get the metadata setting. */
      if (config_lookup_string(config.cfg, "metadata.enabled", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.metadata_enabled = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.metadata_enabled = 1;
        else
          die("Invalid metadata enabled option choice \"%s\". It should be \"yes\" or \"no\"", str);
      }

      if (config_lookup_string(config.cfg, "metadata.include_cover_art", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.get_coverart = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.get_coverart = 1;
        else
          die("Invalid metadata include_cover_art option choice \"%s\". It should be \"yes\" or "
              "\"no\"",
              str);
      }

      if (config_lookup_string(config.cfg, "metadata.pipe_name", &str)) {
        config.metadata_pipename = (char *)str;
      }

      if (config_lookup_float(config.cfg, "metadata.progress_interval", &dvalue)) {
        config.metadata_progress_interval = dvalue;
      }

      if (config_lookup_string(config.cfg, "metadata.socket_address", &str)) {
        config.metadata_sockaddr = (char *)str;
      }
      if (config_lookup_int(config.cfg, "metadata.socket_port", &value)) {
        config.metadata_sockport = value;
      }
      config.metadata_sockmsglength = 500;
      if (config_lookup_int(config.cfg, "metadata.socket_msglength", &value)) {
        config.metadata_sockmsglength = value < 500 ? 500 : value > 65000 ? 65000 : value;
      }

#endif

#ifdef CONFIG_METADATA_HUB
      if (config_lookup_string(config.cfg, "metadata.cover_art_cache_directory", &str)) {
        config.cover_art_cache_dir = (char *)str;
      }

      if (config_lookup_string(config.cfg, "diagnostics.retain_cover_art", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.retain_coverart = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.retain_coverart = 1;
        else
          die("Invalid metadata \"retain_cover_art\" option choice \"%s\". It should be \"yes\" or "
              "\"no\"",
              str);
      }
#endif

      if (config_lookup_string(config.cfg, "sessioncontrol.run_this_before_play_begins", &str)) {
        config.cmd_start = (char *)str;
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.run_this_after_play_ends", &str)) {
        config.cmd_stop = (char *)str;
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.run_this_before_entering_active_state",
                               &str)) {
        config.cmd_active_start = (char *)str;
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.run_this_after_exiting_active_state",
                               &str)) {
        config.cmd_active_stop = (char *)str;
      }

      if (config_lookup_float(config.cfg, "sessioncontrol.active_state_timeout", &dvalue)) {
        if (dvalue < 0.0)
          warn("Invalid value \"%f\" for \"active_state_timeout\". It must be positive. "
               "The default of %f will be used instead.",
               dvalue, config.active_state_timeout);
        else
          config.active_state_timeout = dvalue;
      }

      if (config_lookup_string(config.cfg,
                               "sessioncontrol.run_this_if_an_unfixable_error_is_detected", &str)) {
        config.cmd_unfixable = (char *)str;
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.wait_for_completion", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.cmd_blocking = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.cmd_blocking = 1;
        else
          warn("Invalid \"wait_for_completion\" option choice \"%s\". It should be "
               "\"yes\" or \"no\". It is set to \"no\".",
               str);
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.before_play_begins_returns_output",
                               &str)) {
        if (strcasecmp(str, "no") == 0)
          config.cmd_start_returns_output = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.cmd_start_returns_output = 1;
        else
          die("Invalid \"before_play_begins_returns_output\" option choice \"%s\". It "
              "should be "
              "\"yes\" or \"no\"",
              str);
      }

      if (config_lookup_string(config.cfg, "sessioncontrol.allow_session_interruption", &str)) {
        config.dont_check_timeout = 0; // this is for legacy -- only set by -t 0
        if (strcasecmp(str, "no") == 0)
          config.allow_session_interruption = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.allow_session_interruption = 1;
        else
          die("Invalid \"allow_interruption\" option choice \"%s\". It should be "
              "\"yes\" "
              "or \"no\"",
              str);
      }

      if (config_lookup_int(config.cfg, "sessioncontrol.session_timeout", &value)) {
        config.timeout = value;
        config.dont_check_timeout = 0; // this is for legacy -- only set by -t 0
      }

#ifdef CONFIG_CONVOLUTION
      if (config_lookup_string(config.cfg, "dsp.convolution", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.convolution = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.convolution = 1;
        else
          die("Invalid dsp.convolution setting \"%s\". It should be \"yes\" or \"no\"", str);
      }

      if (config_lookup_float(config.cfg, "dsp.convolution_gain", &dvalue)) {
        config.convolution_gain = dvalue;
        if (dvalue > 10 || dvalue < -50)
          die("Invalid value \"%f\" for dsp.convolution_gain. It should be between -50 and +10 dB",
              dvalue);
      }

      if (config_lookup_int(config.cfg, "dsp.convolution_max_length", &value)) {
        config.convolution_max_length = value;

        if (value < 1 || value > 200000)
          die("dsp.convolution_max_length must be within 1 and 200000");
      }

      if (config_lookup_string(config.cfg, "dsp.convolution_ir_file", &str)) {
        config.convolution_ir_file = strdup(str);
        config.convolver_valid =
            convolver_init(config.convolution_ir_file, config.convolution_max_length);
      }

      if (config.convolution && config.convolution_ir_file == NULL) {
        warn("Convolution enabled but no convolution_ir_file provided");
      }
#endif
      if (config_lookup_string(config.cfg, "dsp.loudness", &str)) {
        if (strcasecmp(str, "no") == 0)
          config.loudness = 0;
        else if (strcasecmp(str, "yes") == 0)
          config.loudness = 1;
        else
          die("Invalid dsp.loudness \"%s\". It should be \"yes\" or \"no\"", str);
      }

      if (config_lookup_float(config.cfg, "dsp.loudness_reference_volume_db", &dvalue)) {
        config.loudness_reference_volume_db = dvalue;
        if (dvalue > 0 || dvalue < -100)
          die("Invalid value \"%f\" for dsp.loudness_reference_volume_db. It should be between "
              "-100 and 0",
              dvalue);
      }

      if (config.loudness == 1 && config_lookup_string(config.cfg, "alsa.mixer_control_name", &str))
        die("Loudness activated but hardware volume is active. You must remove "
            "\"alsa.mixer_control_name\" to use the loudness filter.");

    } else {
      if (config_error_type(&config_file_stuff) == CONFIG_ERR_FILE_IO)
        debug(2, "Error reading configuration file \"%s\": \"%s\".",
              config_error_file(&config_file_stuff), config_error_text(&config_file_stuff));
      else {
        die("Line %d of the configuration file \"%s\":\n%s", config_error_line(&config_file_stuff),
            config_error_file(&config_file_stuff), config_error_text(&config_file_stuff));
      }
    }
#if defined(CONFIG_DBUS_INTERFACE)
    /* Get the dbus service sbus setting. */
    if (config_lookup_string(config.cfg, "general.dbus_service_bus", &str)) {
      if (strcasecmp(str, "system") == 0)
        config.dbus_service_bus_type = DBT_system;
      else if (strcasecmp(str, "session") == 0)
        config.dbus_service_bus_type = DBT_session;
      else
        die("Invalid dbus_service_bus option choice \"%s\". It should be \"system\" (default) or "
            "\"session\"",
            str);
    }
#endif

#if defined(CONFIG_MPRIS_INTERFACE)
    /* Get the mpris service sbus setting. */
    if (config_lookup_string(config.cfg, "general.mpris_service_bus", &str)) {
      if (strcasecmp(str, "system") == 0)
        config.mpris_service_bus_type = DBT_system;
      else if (strcasecmp(str, "session") == 0)
        config.mpris_service_bus_type = DBT_session;
      else
        die("Invalid mpris_service_bus option choice \"%s\". It should be \"system\" (default) or "
            "\"session\"",
            str);
    }
#endif

#ifdef CONFIG_MQTT
    config_set_lookup_bool(config.cfg, "mqtt.enabled", &config.mqtt_enabled);
    if (config.mqtt_enabled && !config.metadata_enabled) {
      die("You need to have metadata enabled in order to use mqtt");
    }
    if (config_lookup_string(config.cfg, "mqtt.hostname", &str)) {
      config.mqtt_hostname = (char *)str;
      // TODO: Document that, if this is false, whole mqtt func is disabled
    }
    config.mqtt_port = 1883;
    if (config_lookup_int(config.cfg, "mqtt.port", &value)) {
      if ((value < 0) || (value > 65535))
        die("Invalid mqtt port number  \"%sd\". It should be between 0 and 65535, default is 1883",
            value);
      else
        config.mqtt_port = value;
    }

    if (config_lookup_string(config.cfg, "mqtt.username", &str)) {
      config.mqtt_username = (char *)str;
    }
    if (config_lookup_string(config.cfg, "mqtt.password", &str)) {
      config.mqtt_password = (char *)str;
    }
    int capath = 0;
    if (config_lookup_string(config.cfg, "mqtt.capath", &str)) {
      config.mqtt_capath = (char *)str;
      capath = 1;
    }
    if (config_lookup_string(config.cfg, "mqtt.cafile", &str)) {
      if (capath)
        die("Supply either mqtt cafile or mqtt capath -- you have supplied both!");
      config.mqtt_cafile = (char *)str;
    }
    int certkeynum = 0;
    if (config_lookup_string(config.cfg, "mqtt.certfile", &str)) {
      config.mqtt_certfile = (char *)str;
      certkeynum++;
    }
    if (config_lookup_string(config.cfg, "mqtt.keyfile", &str)) {
      config.mqtt_keyfile = (char *)str;
      certkeynum++;
    }
    if (certkeynum != 0 && certkeynum != 2) {
      die("If you want to use TLS Client Authentication, you have to specify "
          "mqtt.certfile AND mqtt.keyfile.\nYou have supplied only one of them.\n"
          "If you do not want to use TLS Client Authentication, leave both empty.");
    }

    if (config_lookup_string(config.cfg, "mqtt.topic", &str)) {
      config.mqtt_topic = (char *)str;
    }
    config_set_lookup_bool(config.cfg, "mqtt.publish_raw", &config.mqtt_publish_raw);
    config_set_lookup_bool(config.cfg, "mqtt.publish_parsed", &config.mqtt_publish_parsed);
    config_set_lookup_bool(config.cfg, "mqtt.publish_cover", &config.mqtt_publish_cover);
    if (config.mqtt_publish_cover && !config.get_coverart) {
      die("You need to have metadata.include_cover_art enabled in order to use mqtt.publish_cover");
    }
    config_set_lookup_bool(config.cfg, "mqtt.enable_remote", &config.mqtt_enable_remote);
    if (config_lookup_string(config.cfg, "mqtt.empty_payload_substitute", &str)) {
      if (strlen(str) == 0)
        config.mqtt_empty_payload_substitute = NULL;
      else
        config.mqtt_empty_payload_substitute = strdup(str);
    } else {
      config.mqtt_empty_payload_substitute = strdup("--");
    }
#ifndef CONFIG_AVAHI
    if (config.mqtt_enable_remote) {
      die("You have enabled MQTT remote control which requires shairport-sync to be built with "
          "Avahi, but your installation is not using avahi. Please reinstall/recompile with "
          "avahi enabled, or disable remote control.");
    }
#endif
#endif

#ifdef CONFIG_AIRPLAY_2
    long long aid;

    // replace the airplay_device_id with this, if provided
    if (config_lookup_int64(config.cfg, "general.airplay_device_id", &aid)) {
      temporary_airplay_id = aid;
    }

    // add the airplay_device_id_offset if provided
    if (config_lookup_int64(config.cfg, "general.airplay_device_id_offset", &aid)) {
      temporary_airplay_id += aid;
    }

#endif
  }

  // now, do the command line options again, but this time do them fully -- it's a unix convention
  // that command line
  // arguments have precedence over configuration file settings.
  optind = argc;
  for (j = 0; j < argc; j++)
    if (strcmp(argv[j], "--") == 0)
      optind = j;

  optCon = poptGetContext(NULL, optind, (const char **)argv, optionsTable, 0);
  if (optCon == NULL)
    die("Can not get a popt context.");
  poptSetOtherOptionHelp(optCon, "[OPTIONS]* ");

  /* Now do options processing, get portname */
  int tdebuglev = 0;
  while ((c = poptGetNextOpt(optCon)) >= 0) {
    switch (c) {
    case 'v':
      tdebuglev++;
      break;
    case 't':
      if (config.timeout == 0) {
        config.dont_check_timeout = 1;
        config.allow_session_interruption = 1;
      } else {
        config.dont_check_timeout = 0;
        config.allow_session_interruption = 0;
      }
      break;
#ifdef CONFIG_METADATA
    case 'M':
      config.metadata_enabled = 1;
      break;
    case 'g':
      if (config.metadata_enabled == 0)
        die("If you want to get cover art, ensure metadata_enabled is true.");
      break;
#endif
    case 'S':
      if (strcmp(stuffing, "basic") == 0)
        config.packet_stuffing = ST_basic;
      else if (strcmp(stuffing, "auto") == 0)
        config.packet_stuffing = ST_auto;
      else if (strcmp(stuffing, "soxr") == 0)
#ifdef CONFIG_SOXR
        config.packet_stuffing = ST_soxr;
#else
        die("The soxr option not available because this version of shairport-sync was built "
            "without libsoxr "
            "support. Change the -S option setting.");
#endif
      else
        die("Illegal stuffing option \"%s\" -- must be \"basic\" or \"soxr\"", stuffing);
      break;
    }
  }
  if (c < -1) {
    die("%s: %s", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
  }

  poptFreeContext(optCon);

  // here, we are finally finished reading the options

  // finish the Airplay 2 options

#ifdef CONFIG_AIRPLAY_2

  char shared_memory_interface_name[256] = "";
  snprintf(shared_memory_interface_name, sizeof(shared_memory_interface_name), "/%s-%" PRIx64 "",
           config.appName, temporary_airplay_id);
  // debug(1, "smi name: \"%s\"", shared_memory_interface_name);

  config.nqptp_shared_memory_interface_name = strdup(NQPTP_INTERFACE_NAME);

  char apids[6 * 2 + 5 + 1]; // six pairs of digits, 5 colons and a NUL
  apids[6 * 2 + 5] = 0;      // NUL termination
  int i;
  char hexchar[] = "0123456789abcdef";
  for (i = 5; i >= 0; i--) {
    // In AirPlay 2 mode, the AP1 name prefix must be
    // the same as the AirPlay 2 device id less the colons.
    config.ap1_prefix[i] = temporary_airplay_id & 0xFF; 
    apids[i * 3 + 1] = hexchar[temporary_airplay_id & 0xF];
    temporary_airplay_id = temporary_airplay_id >> 4;
    apids[i * 3] = hexchar[temporary_airplay_id & 0xF];
    temporary_airplay_id = temporary_airplay_id >> 4;
    if (i != 0)
      apids[i * 3 - 1] = ':';
  }

  config.airplay_device_id = strdup(apids);

#ifdef CONFIG_METADATA
  // If we are asking for metadata, turn on the relevant bits
  if (config.metadata_enabled != 0) {
    config.airplay_features |= (1 << 17) | (1 << 16); // 16 is progress, 17 is text
    // If we are asking for artwork, turn on the relevant bit
    if (config.get_coverart)
      config.airplay_features |= (1 << 15); // 15 is artwork
  }
#endif

#endif

#ifdef CONFIG_LIBDAEMON
  if ((daemonisewith) && (daemonisewithout))
    die("Select either daemonize_with_pid_file or daemonize_without_pid_file -- you have selected "
        "both!");
  if ((daemonisewith) || (daemonisewithout)) {
    config.daemonise = 1;
    if (daemonisewith)
      config.daemonise_store_pid = 1;
  };
#else
  /* Check if we are called with -d or --daemon or -j or justDaemoniseNoPIDFile options*/
  if ((daemonisewith != 0) || (daemonisewithout != 0)) {
    fprintf(stderr,
            "%s was built without libdaemon, so does not support daemonisation using the "
            "-d, --daemon, -j or --justDaemoniseNoPIDFile options\n",
            config.appName);
    exit(EXIT_FAILURE);
  }

#endif

#ifdef CONFIG_METADATA
  if ((config.metadata_enabled == 1) && (config.metadata_pipename == NULL)) {
    char temp_metadata_pipe_name[4096];
    strcpy(temp_metadata_pipe_name, "/tmp/");
    strcat(temp_metadata_pipe_name, config.appName);
    strcat(temp_metadata_pipe_name, "-metadata");
    config.metadata_pipename = strdup(temp_metadata_pipe_name);
    debug(2, "default metadata_pipename is \"%s\".", temp_metadata_pipe_name);
  }
#endif

  /* if the regtype hasn't been set, do it now */
  if (config.regtype == NULL)
    config.regtype = strdup("_raop._tcp");
#ifdef CONFIG_AIRPLAY_2
  if (config.regtype2 == NULL)
    config.regtype2 = strdup("_airplay._tcp");
#endif

  if (tdebuglev != 0)
    debuglev = tdebuglev;

  // now set the initial volume to the default volume
  config.airplay_volume =
      config.default_airplay_volume; // if no volume is ever set or requested, default to initial
                                     // default value if nothing else comes in first.
  // now, do the substitutions in the service name
  char hostname[100];
  gethostname(hostname, 100);

  // strip off a terminating .<anything>, e.g. .local from the hostname
  char *last_dot = strrchr(hostname, '.');
  if (last_dot != NULL)
    *last_dot = '\0';

  char *i0;
  if (raw_service_name == NULL)
    i0 = strdup("%H"); // this is the default it the Service Name wasn't specified
  else
    i0 = strdup(raw_service_name);

  // here, do the substitutions for %h, %H, %v and %V
  char *i1 = str_replace(i0, "%h", hostname);
  if ((hostname[0] >= 'a') && (hostname[0] <= 'z'))
    hostname[0] = hostname[0] - 0x20; // convert a lowercase first letter into a capital letter
  char *i2 = str_replace(i1, "%H", hostname);
  char *i3 = str_replace(i2, "%v", PACKAGE_VERSION);
  char *vs = get_version_string();
  config.service_name = str_replace(i3, "%V", vs); // service name complete
  free(i0);
  free(i1);
  free(i2);
  free(i3);
  free(vs);

#ifdef CONFIG_MQTT
  // mqtt topic was not set. As we have the service name just now, set it
  if (config.mqtt_topic == NULL) {
    int topic_length = 1 + strlen(config.service_name) + 1;
    char *topic = malloc(topic_length + 1);
    snprintf(topic, topic_length, "/%s/", config.service_name);
    config.mqtt_topic = topic;
  }
#endif

#ifdef CONFIG_LIBDAEMON

// now, check and calculate the pid directory
#ifdef DEFINED_CUSTOM_PID_DIR
  char *use_this_pid_dir = PIDDIR;
#else
  char temp_pid_dir[4096];
  strcpy(temp_pid_dir, "/var/run/");
  strcat(temp_pid_dir, config.appName);
  debug(3, "Default PID directory is \"%s\".", temp_pid_dir);
  char *use_this_pid_dir = temp_pid_dir;
#endif
  // debug(1,"config.piddir \"%s\".",config.piddir);
  if (config.piddir)
    use_this_pid_dir = config.piddir;
  if (use_this_pid_dir)
    config.computed_piddir = strdup(use_this_pid_dir);
#endif
  return optind + 1;
}

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
static GMainLoop *g_main_loop = NULL;

pthread_t dbus_thread;
void *dbus_thread_func(__attribute__((unused)) void *arg) {
  g_main_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(g_main_loop);
  debug(2, "g_main_loop thread exit");
  pthread_exit(NULL);
}
#endif

#ifdef CONFIG_LIBDAEMON
char pid_file_path_string[4096] = "\0";

const char *pid_file_proc(void) {
  snprintf(pid_file_path_string, sizeof(pid_file_path_string), "%s/%s.pid", config.computed_piddir,
           daemon_pid_file_ident ? daemon_pid_file_ident : "unknown");
  debug(1, "PID file: \"%s\".", pid_file_path_string);
  return pid_file_path_string;
}
#endif

void exit_rtsp_listener() {
  pthread_cancel(rtsp_listener_thread);
  pthread_join(rtsp_listener_thread, NULL); // not sure you need this
}

void exit_function() {

  if (type_of_exit_cleanup != TOE_emergency) {
    // the following is to ensure that if libdaemon has been included
    // that most of this code will be skipped when the parent process is exiting
    // exec
#ifdef CONFIG_LIBDAEMON
    if ((this_is_the_daemon_process) ||
        (config.daemonise == 0)) { // if this is the daemon process that is exiting or it's not
                                   // actually daemonised at all
#endif
      debug(2, "exit function called...");
      /*
      Actually, there is no terminate_mqtt() function.
      #ifdef CONFIG_MQTT
              if (config.mqtt_enabled) {
                      terminate_mqtt();
              }
      #endif
      */

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
      /*
      Actually, there is no stop_mpris_service() function.
      #ifdef CONFIG_MPRIS_INTERFACE
              stop_mpris_service();
      #endif
      */
#ifdef CONFIG_DBUS_INTERFACE
      debug(2, "Stopping D-Bus service");
      stop_dbus_service();
      debug(2, "Stopping D-Bus service done");
#endif
      if (g_main_loop) {
        debug(2, "Stopping D-Bus Loop Thread");
        g_main_loop_quit(g_main_loop);

        // If the request to exit has come from the D-Bus system,
        // the D-Bus Loop Thread will not exit until the request is completed
        // so don't wait for it
        if (type_of_exit_cleanup != TOE_dbus)
          pthread_join(dbus_thread, NULL);
        debug(2, "Stopping D-Bus Loop Thread Done");
      }
#endif

#ifdef CONFIG_DACP_CLIENT
      debug(2, "Stopping DACP Monitor");
      dacp_monitor_stop();
      debug(2, "Stopping DACP Monitor Done");
#endif

#ifdef CONFIG_METADATA_HUB
      debug(2, "Stopping metadata hub");
      metadata_hub_stop();
      debug(2, "Stopping metadata done");
#endif

#ifdef CONFIG_METADATA
      debug(2, "Stopping metadata");
      metadata_stop(); // close down the metadata pipe
      debug(2, "Stopping metadata done");
#endif
      debug(2, "Stopping the activity monitor.");
      activity_monitor_stop(0);
      debug(2, "Stopping the activity monitor done.");

      if ((config.output) && (config.output->deinit)) {
        debug(2, "Deinitialise the audio backend.");
        config.output->deinit();
        debug(2, "Deinitialise the audio backend done.");
      }

#ifdef CONFIG_SOXR
      // be careful -- not sure if the thread can be cancelled cleanly, so wait for it to shut down
      if (soxr_time_check_thread_started != 0) {
        debug(2, "Waiting for SoXr timecheck to terminate...");
        pthread_join(soxr_time_check_thread, NULL);
        soxr_time_check_thread_started = 0;
        debug(2, "Waiting for SoXr timecheck to terminate done");
      }

#endif

      if (conns)
        free(conns); // make sure the connections have been deleted first

      if (config.service_name)
        free(config.service_name);

#ifdef CONFIG_MQTT
      if (config.mqtt_empty_payload_substitute)
        free(config.mqtt_empty_payload_substitute);
#endif

#ifdef CONFIG_CONVOLUTION
      if (config.convolution_ir_file)
        free(config.convolution_ir_file);
#endif

      if (config.regtype)
        free(config.regtype);
#ifdef CONFIG_AIRPLAY_2
      if (config.regtype2)
        free(config.regtype2);
      if (config.nqptp_shared_memory_interface_name)
        free(config.nqptp_shared_memory_interface_name);
      if (config.airplay_device_id)
        free(config.airplay_device_id);
      if (config.airplay_pin)
        free(config.airplay_pin);
      if (config.airplay_pi)
        free(config.airplay_pi);
      ptp_shm_interface_close(); // close it if it's open
#endif

#ifdef CONFIG_LIBDAEMON
      if (this_is_the_daemon_process) {
        daemon_retval_send(0);
        daemon_pid_file_remove();
        daemon_signal_done();
        if (config.computed_piddir)
          free(config.computed_piddir);
      }
    }
#endif
    if (config.cfg)
      config_destroy(config.cfg);
    if (config_file_real_path)
      free(config_file_real_path);
    if (config.appName)
      free(config.appName);

      // probably should be freeing malloc'ed memory here, including strdup-created strings...

#ifdef CONFIG_LIBDAEMON
    if (this_is_the_daemon_process) { // this is the daemon that is exiting
      debug(1, "libdaemon daemon process exit");
    } else {
      if (config.daemonise)
        debug(1, "libdaemon parent process exit");
      else
        debug(1, "normal exit");
    }
#else
    mdns_unregister(); // once the dacp handler is done and all player threrads are done it should
                       // be safe
    debug(1, "normal exit");
#endif
  } else {
    debug(1, "emergency exit");
  }
}

// for removing zombie script processes
// see: http://www.microhowto.info/howto/reap_zombie_processes_using_a_sigchld_handler.html
// used with thanks.

void handle_sigchld(__attribute__((unused)) int sig) {
  int saved_errno = errno;
  while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) {
  }
  errno = saved_errno;
}

// for clean exits
void intHandler(__attribute__((unused)) int k) {
  debug(2, "exit on SIGINT");
  exit(EXIT_SUCCESS);
}

void termHandler(__attribute__((unused)) int k) {
  debug(2, "exit on SIGTERM");
  exit(EXIT_SUCCESS);
}

void _display_config(const char *filename, const int linenumber, __attribute__((unused)) int argc,
                     __attribute__((unused)) char **argv) {
  _inform(filename, linenumber, ">> Display Config Start.");

  // see the man entry on popen
  FILE *fp;
  int status;
  char result[1024];

  fp = popen("uname -a 2>/dev/null", "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From \"uname -a\":");
      if (result[strlen(result) - 1] <= ' ')
        result[strlen(result) - 1] = '\0'; // remove the last character if it's not printable
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  fp = popen("(cat /etc/os-release | grep PRETTY_NAME | sed 's/PRETTY_NAME=//' | sed 's/\"//g') "
             "2>/dev/null",
             "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From /etc/os-release:");
      if (result[strlen(result) - 1] <= ' ')
        result[strlen(result) - 1] = '\0'; // remove the last character if it's not printable
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  fp = popen("cat /sys/firmware/devicetree/base/model 2>/dev/null", "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From /sys/firmware/devicetree/base/model:");
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  char *version_string = get_version_string();
  if (version_string) {
    _inform(filename, linenumber, "");
    _inform(filename, linenumber, "Shairport Sync Version String:");
    _inform(filename, linenumber, " %s", version_string);
    free(version_string);
  } else {
    debug(1, "Can't print version string!\n");
  }

  if (argc != 0) {
    char *obfp = result;
    int i;
    for (i = 0; i < argc - 1; i++) {
      snprintf(obfp, strlen(argv[i]) + 2, "%s ", argv[i]);
      obfp += strlen(argv[i]) + 1;
    }
    snprintf(obfp, strlen(argv[i]) + 1, "%s", argv[i]);
    obfp += strlen(argv[i]);
    *obfp = 0;

    _inform(filename, linenumber, "");
    _inform(filename, linenumber, "Command Line:");
    _inform(filename, linenumber, " %s", result);
  }

  if (config.cfg == NULL)
    _inform(filename, linenumber, "No configuration file.");
  else {
    int configpipe[2];
    if (pipe(configpipe) == 0) {
      FILE *cw;
      cw = fdopen(configpipe[1], "w");
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "Configuration File:");
      _inform(filename, linenumber, " %s", config_file_real_path);
      _inform(filename, linenumber, "");
      config_write(config.cfg, cw);
      fclose(cw);
      // get back the raw configuration file settings text
      FILE *cr;
      cr = fdopen(configpipe[0], "r");
      int i = 0;
      int ch = 0;
      do {
        ch = fgetc(cr);
        if (ch == EOF) {
          result[i] = '\0';
        } else {
          result[i] = (char)ch;
          i++;
        }
      } while (ch != EOF);
      fclose(cr);
      // debug(1,"result is \"%s\".",result);
      // remove empty stanzas
      char *i0 = str_replace(result, "general : \n{\n};\n", "");
      char *i1 = str_replace(i0, "sessioncontrol : \n{\n};\n", "");
      char *i2 = str_replace(i1, "alsa : \n{\n};\n", "");
      char *i3 = str_replace(i2, "sndio : \n{\n};\n", "");
      char *i4 = str_replace(i3, "pa : \n{\n};\n", "");
      char *i5 = str_replace(i4, "jack : \n{\n};\n", "");
      char *i6 = str_replace(i5, "pipe : \n{\n};\n", "");
      char *i7 = str_replace(i6, "dsp : \n{\n};\n", "");
      char *i8 = str_replace(i7, "metadata : \n{\n};\n", "");
      char *i9 = str_replace(i8, "mqtt : \n{\n};\n", "");
      char *i10 = str_replace(i9, "diagnostics : \n{\n};\n", "");
      // debug(1,"i10 is \"%s\".",i10);

      // free intermediate strings
      free(i9);
      free(i8);
      free(i7);
      free(i6);
      free(i5);
      free(i4);
      free(i3);
      free(i2);
      free(i1);
      free(i0);

      // print it out
      if (strlen(i10) == 0)
        _inform(filename, linenumber, "The Configuration file contains no active settings.");
      else {
        _inform(filename, linenumber, "Configuration File Settings:");
        char *p = i10;
        while (*p != '\0') {
          i = 0;
          while ((*p != '\0') && (*p != '\n')) {
            result[i] = *p;
            p++;
            i++;
          }
          if (i != 0) {
            result[i] = '\0';
            _inform(filename, linenumber, " %s", result);
          }
          if (*p == '\n')
            p++;
        }
      }

      free(i10); // free the cleaned-up configuration string

      /*
            while (fgets(result, 1024, cr) != NULL) {
              // replace funny character at the end, if it's there
              if (result[strlen(result) - 1] <= ' ')
                result[strlen(result) - 1] = '\0'; // remove the last character if it's not
         printable _inform(filename, linenumber, " %s", result);
            }
      */
    } else {
      debug(1, "Error making pipe.\n");
    }
  }
  _inform(filename, linenumber, "");
  _inform(filename, linenumber, ">> Display Config End.");
}

#define display_config(argc, argv) _display_config(__FILE__, __LINE__, argc, argv)

int main(int argc, char **argv) {
  memset(&config, 0, sizeof(config)); // also clears all strings, BTW
  /* Check if we are called with -V or --version parameter */
  if (argc >= 2 && ((strcmp(argv[1], "-V") == 0) || (strcmp(argv[1], "--version") == 0))) {
    print_version();
    exit(EXIT_SUCCESS);
  }

  // this is a bit weird, but necessary -- basename() may modify the argument passed in
  char *basec = strdup(argv[0]);
  char *bname = basename(basec);
  config.appName = strdup(bname);
  if (config.appName == NULL)
    die("can not allocate memory for the app name!");
  free(basec);

  strcpy(configuration_file_path, SYSCONFDIR);
  // strcat(configuration_file_path, "/shairport-sync"); // thinking about adding a special
  // shairport-sync directory
  strcat(configuration_file_path, "/");
  strcat(configuration_file_path, config.appName);
  strcat(configuration_file_path, ".conf");
  config.configfile = configuration_file_path;

#ifdef CONFIG_AIRPLAY_2
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(53, 10, 0)
  avcodec_init();
#endif
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
  avcodec_register_all();
#endif
#endif

  /* Check if we are called with -h or --help parameter */
  if (argc >= 2 && ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0))) {
    usage(argv[0]);
    exit(EXIT_SUCCESS);
  }

  /* Check if we are called with -log-to-syslog */
  if (argc >= 2 && (strcmp(argv[1], "--log-to-syslog") == 0)) {
    log_to_syslog_select_is_first_command_line_argument = 1;
    log_to_syslog();
  } else {
    log_to_stderr();
  }

  pid = getpid();
  config.log_fd = -1;
  conns = NULL; // no connections active
  ns_time_at_startup = get_absolute_time_in_ns();
  ns_time_at_last_debug_message = ns_time_at_startup;

#ifdef CONFIG_LIBDAEMON
  daemon_set_verbosity(LOG_DEBUG);
#else
  setlogmask(LOG_UPTO(LOG_DEBUG));
  openlog(NULL, 0, LOG_DAEMON);
#endif
  type_of_exit_cleanup = TOE_normal; // what kind of exit cleanup needed
  atexit(exit_function);

  // set defaults

  // get a device id -- the first non-local MAC address
  get_device_id((uint8_t *)&config.hw_addr, 6);

  // get the endianness
  union {
    uint32_t u32;
    uint8_t arr[4];
  } xn;

  xn.arr[0] = 0x44; /* Lowest-address byte */
  xn.arr[1] = 0x33;
  xn.arr[2] = 0x22;
  xn.arr[3] = 0x11; /* Highest-address byte */

  if (xn.u32 == 0x11223344)
    config.endianness = SS_LITTLE_ENDIAN;
  else if (xn.u32 == 0x33441122)
    config.endianness = SS_PDP_ENDIAN;
  else if (xn.u32 == 0x44332211)
    config.endianness = SS_BIG_ENDIAN;
  else
    die("Can not recognise the endianness of the processor.");

  // set non-zero / non-NULL default values here
  // but note that audio back ends also have a chance to set defaults

  // get the first output backend in the list and make it the default
  audio_output *first_backend = audio_get_output(NULL);
  if (first_backend == NULL) {
    die("No audio backend found! Check your build of Shairport Sync.");
  } else {
    strncpy(first_backend_name, first_backend->name, sizeof(first_backend_name) - 1);
    config.output_name = first_backend_name;
  }

  // config.statistics_requested = 0; // don't print stats in the log
  // config.userSuppliedLatency = 0; // zero means none supplied

  config.debugger_show_file_and_line =
      1; // by default, log the file and line of the originating message
  config.debugger_show_relative_time =
      1;                // by default, log the  time back to the previous debug message
  config.timeout = 120; // this number of seconds to wait for [more] audio before switching to idle.
  config.buffer_start_fill = 220;

  config.resync_threshold = 0.050;   // default
  config.resync_recovery_time = 0.1; // drop this amount of frames following the resync delay.
  config.tolerance = 0.002;

#ifdef CONFIG_AIRPLAY_2
  config.timeout = 0; // disable watchdog
  config.port = 7000;
#else
  config.port = 5000;
#endif

#ifdef CONFIG_SOXR
  config.packet_stuffing = ST_auto; // use soxr interpolation by default if support has been
                                    // included and if the CPU is fast enough
#else
  config.packet_stuffing = ST_basic; // simple interpolation or deletion
#endif

  // char hostname[100];
  // gethostname(hostname, 100);
  // config.service_name = malloc(20 + 100);
  // snprintf(config.service_name, 20 + 100, "Shairport Sync on %s", hostname);
  set_requested_connection_state_to_output(
      1); // we expect to be able to connect to the output device
  config.audio_backend_buffer_desired_length = 0.15; // seconds
  config.udp_port_base = 6001;
  config.udp_port_range = 10;
  config.output_format = SPS_FORMAT_S16_LE; // default
  config.output_format_auto_requested = 1;  // default auto select format
  config.output_rate = 44100;               // default
  config.output_rate_auto_requested = 1;    // default auto select format
  config.decoders_supported =
      1 << decoder_hammerton; // David Hammerton's decoder supported by default
#ifdef CONFIG_APPLE_ALAC
  config.decoders_supported += 1 << decoder_apple_alac;
  config.use_apple_decoder = 1; // use the ALAC decoder by default if support has been included
#endif

  // initialise random number generator

  r64init(0);

#ifdef CONFIG_LIBDAEMON

  /* Reset signal handlers */
  if (daemon_reset_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to reset all signal handlers: %s", strerror(errno));
    return 1;
  }

  /* Unblock signals */
  if (daemon_unblock_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to unblock all signals: %s", strerror(errno));
    return 1;
  }

  /* Set identification string for the daemon for both syslog and PID file */
  daemon_pid_file_ident = daemon_log_ident = daemon_ident_from_argv0(argv[0]);

  daemon_pid_file_proc = pid_file_proc;

#endif
  // parse arguments into config -- needed to locate pid_dir
  int audio_arg = parse_options(argc, argv);

  // mDNS supports maximum of 63-character names (we append 13).
  if (strlen(config.service_name) > 50) {
    warn("The service name \"%s\" is too long (max 50 characters) and has been truncated.",
         config.service_name);
    config.service_name[50] = '\0'; // truncate it and carry on...
  }

  if (display_config_selected != 0) {
    display_config(argc, argv);
    if (argc == 2) {
      inform(">> Goodbye!");
      exit(EXIT_SUCCESS);
    }
  }

  /* Check if we are called with -k or --kill option */
  if (killOption != 0) {
#ifdef CONFIG_LIBDAEMON
    int ret;

    /* Kill daemon with SIGTERM */
    /* Check if the new function daemon_pid_file_kill_wait() is available, if it is, use it. */
    if ((ret = daemon_pid_file_kill_wait(SIGTERM, 5)) < 0) {
      if (errno == ENOENT)
        warn("Failed to kill the %s daemon. The PID file was not found.", config.appName);
      // daemon_log(LOG_WARNING, "Failed to kill %s daemon: PID file not found.", config.appName);
      else
        warn("Failed to kill the %s daemon. Error: \"%s\", errno %u.", config.appName,
             strerror(errno), errno);
      // daemon_log(LOG_WARNING, "Failed to kill %s daemon: \"%s\", errno %u.", config.appName,
      //            strerror(errno), errno);
    }
    return ret < 0 ? 1 : 0;
#else
    warn("%s was built without libdaemon, so it does not support the -k or --kill option.",
         config.appName);
    return 1;
#endif
  }

#ifdef CONFIG_LIBDAEMON
  /* If we are going to daemonise, check that the daemon is not running already.*/
  if ((config.daemonise) && ((pid = daemon_pid_file_is_running()) >= 0)) {
    warn("The %s deamon is already running with process ID (PID) %u.", config.appName, pid);
    // daemon_log(LOG_ERR, "The %s daemon is already running as PID %u", config.appName, pid);
    return 1;
  }

  /* here, daemonise with libdaemon */

  if (config.daemonise) {
    /* Prepare for return value passing from the initialization procedure of the daemon process */
    if (daemon_retval_init() < 0) {
      die("Failed to create pipe.");
    }

    /* Do the fork */
    if ((pid = daemon_fork()) < 0) {

      /* Exit on error */
      daemon_retval_done();
      return 1;

    } else if (pid) { /* The parent */
      int ret;

      /* Wait for 20 seconds for the return value passed from the daemon process */
      if ((ret = daemon_retval_wait(20)) < 0) {
        die("Could not receive return value from daemon process: %s", strerror(errno));
      }

      switch (ret) {
      case 0:
        break;
      case 1:
        warn("The %s daemon failed to launch: could not close open file descriptors after forking.",
             config.appName);
        break;
      case 2:
        warn("The %s daemon failed to launch: could not create PID file.", config.appName);
        break;
      case 3:
        warn("The %s daemon failed to launch: could not create or access PID directory.",
             config.appName);
        break;
      default:
        warn("The %s daemon failed to launch, error %i.", config.appName, ret);
      }
      return ret;
    } else { /* pid == 0 means we are the daemon */

      this_is_the_daemon_process = 1;
      if (log_to_default != 0) // if a specific logging mode has not been selected
        log_to_syslog();       // automatically send logs to the daemon_log

      /* Close FDs */
      if (daemon_close_all(-1) < 0) {
        warn("Failed to close all file descriptors while daemonising. Error: %s", strerror(errno));
        /* Send the error condition to the parent process */
        daemon_retval_send(1);
        daemon_signal_done();
        return 0;
      }

      /* Create the PID file if required */
      if (config.daemonise_store_pid) {
        /* Create the PID directory if required -- we don't really care about the result */
        debug(1, "PID directory is \"%s\".", config.computed_piddir);
        int result = mkpath(config.computed_piddir, 0700);
        if ((result != 0) && (result != -EEXIST)) {
          // error creating or accessing the PID file directory
          warn("Failed to create the directory \"%s\" for the PID file. Error: %s.",
               config.computed_piddir, strerror(errno));
          daemon_retval_send(3);
          daemon_signal_done();
          return 0;
        }

        if (daemon_pid_file_create() < 0) {
          // daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
          warn("Failed to create the PID file. Error: %s.", strerror(errno));
          daemon_retval_send(2);
          daemon_signal_done();
          return 0;
        }
      }

      /* Send OK to parent process */
      daemon_retval_send(0);
    }
    /* end libdaemon stuff */
  }

#endif

#ifdef CONFIG_AIRPLAY_2

  if (has_fltp_capable_aac_decoder() == 0) {
    die("Shairport Sync can not run on this system. Run \"shairport-sync -h\" for more "
        "information.");
  }

  uint64_t apf = config.airplay_features;
  uint64_t apfh = config.airplay_features;
  apfh = apfh >> 32;
  uint32_t apf32 = apf;
  uint32_t apfh32 = apfh;
  debug(1, "Startup in AirPlay 2 mode, with features 0x%" PRIx32 ",0x%" PRIx32 " on device \"%s\".",
        apf32, apfh32, config.airplay_device_id);
#else
  debug(1, "Startup in classic Airplay (aka \"AirPlay 1\") mode.");
#endif

  // control-c (SIGINT) cleanly
  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = intHandler;
  sigaction(SIGINT, &act, NULL);

  // terminate (SIGTERM)
  struct sigaction act2;
  memset(&act2, 0, sizeof(struct sigaction));
  act2.sa_handler = termHandler;
  sigaction(SIGTERM, &act2, NULL);

  // stop a pipe signal from killing the program
  signal(SIGPIPE, SIG_IGN);

  // install a zombie process reaper
  // see: http://www.microhowto.info/howto/reap_zombie_processes_using_a_sigchld_handler.html
  struct sigaction sa;
  sa.sa_handler = &handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, 0) == -1) {
    perror(0);
    exit(1);
  }

  // make sure the program can create files that group and world can read
  umask(S_IWGRP | S_IWOTH);

  /* print out version */

  char *version_dbs = get_version_string();
  if (version_dbs) {
    debug(1, "Version String: \"%s\"", version_dbs);
    free(version_dbs);
  } else {
    debug(1, "Can't print the version information!");
  }

  // print command line

  if (argc != 0) {
    char result[1024];
    char *obfp = result;
    int i;
    for (i = 0; i < argc - 1; i++) {
      snprintf(obfp, strlen(argv[i]) + 2, "%s ", argv[i]);
      obfp += strlen(argv[i]) + 1;
    }
    snprintf(obfp, strlen(argv[i]) + 1, "%s", argv[i]);
    obfp += strlen(argv[i]);
    *obfp = 0;
    debug(1, "Command Line: \"%s\".", result);
  }

#ifdef CONFIG_AIRPLAY_2
  if (sodium_init() < 0) {
    debug(1, "Can't initialise libsodium!");
  } else {
    debug(2, "libsodium initialised.");
  }

  // this code is based on
  // https://www.gnupg.org/documentation/manuals/gcrypt/Initializing-the-library.html

  /* Version check should be the very first call because it
    makes sure that important subsystems are initialized.
    #define NEED_LIBGCRYPT_VERSION to the minimum required version. */

#define NEED_LIBGCRYPT_VERSION "1.5.4"

  if (!gcry_check_version(NEED_LIBGCRYPT_VERSION)) {
    die("libgcrypt is too old (need %s, have %s).", NEED_LIBGCRYPT_VERSION,
        gcry_check_version(NULL));
  }

  /* Disable secure memory.  */
  gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

  /* ... If required, other initialization goes here.  */

  /* Tell Libgcrypt that initialization has completed. */
  gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

  debug(2, "libgcrypt initialised.");

#endif

  debug(1, "Log Verbosity is %d.", debuglev);

  config.output = audio_get_output(config.output_name);
  if (!config.output) {
    die("Invalid audio backend \"%s\" selected!",
        config.output_name == NULL ? "<unspecified>" : config.output_name);
  }
  config.output->init(argc - audio_arg, argv + audio_arg);

  // pthread_cleanup_push(main_cleanup_handler, NULL);

  // daemon_log(LOG_NOTICE, "startup");

  switch (config.endianness) {
  case SS_LITTLE_ENDIAN:
    debug(2, "The processor is running little-endian.");
    break;
  case SS_BIG_ENDIAN:
    debug(2, "The processor is running big-endian.");
    break;
  case SS_PDP_ENDIAN:
    debug(2, "The processor is running pdp-endian.");
    break;
  }

  /* Mess around with the latency options */
  // Basically, we expect the source to set the latency and add a fixed offset of 11025 frames to
  // it, which sounds right
  // If this latency is outside the max and min latensies that may be set by the source, clamp it to
  // fit.

  // If they specify a non-standard latency, we suggest the user to use the
  // audio_backend_latency_offset instead.

  if (config.userSuppliedLatency) {
    inform("The fixed latency setting is deprecated, as Shairport Sync gets the correct "
           "latency automatically from the source.");
    inform("Use the audio_backend_latency_offset_in_seconds setting "
           "instead to compensate for timing issues.");
    if ((config.userSuppliedLatency != 0) &&
        ((config.userSuppliedLatency < 4410) ||
         (config.userSuppliedLatency > BUFFER_FRAMES * 352 - 22050)))
      die("An out-of-range fixed latency has been specified. It must be between 4410 and %d (at "
          "44100 frames per second).",
          BUFFER_FRAMES * 352 - 22050);
  }

  /* Print out options */
  debug(1, "disable_resend_requests is %s.", config.disable_resend_requests ? "on" : "off");
  debug(1,
        "diagnostic_drop_packet_fraction is %f. A value of 0.0 means no packets will be dropped "
        "deliberately.",
        config.diagnostic_drop_packet_fraction);
  debug(1, "statistics_requester status is %d.", config.statistics_requested);
#if CONFIG_LIBDAEMON
  debug(1, "daemon status is %d.", config.daemonise);
  debug(1, "daemon pid file path is \"%s\".", pid_file_proc());
#endif
  debug(1, "rtsp listening port is %d.", config.port);
  debug(1, "udp base port is %d.", config.udp_port_base);
  debug(1, "udp port range is %d.", config.udp_port_range);
  debug(1, "player name is \"%s\".", config.service_name);
  debug(1, "backend is \"%s\".", config.output_name);
  debug(1, "run_this_before_play_begins action is \"%s\".", strnull(config.cmd_start));
  debug(1, "run_this_after_play_ends action is \"%s\".", strnull(config.cmd_stop));
  debug(1, "wait-cmd status is %d.", config.cmd_blocking);
  debug(1, "run_this_before_play_begins may return output is %d.", config.cmd_start_returns_output);
  debug(1, "run_this_if_an_unfixable_error_is_detected action is \"%s\".",
        strnull(config.cmd_unfixable));
  debug(1, "run_this_before_entering_active_state action is  \"%s\".",
        strnull(config.cmd_active_start));
  debug(1, "run_this_after_exiting_active_state action is  \"%s\".",
        strnull(config.cmd_active_stop));
  debug(1, "active_state_timeout is  %f seconds.", config.active_state_timeout);
  debug(1, "mdns backend \"%s\".", strnull(config.mdns_name));
  debug(2, "userSuppliedLatency is %d.", config.userSuppliedLatency);
  debug(1, "interpolation setting is \"%s\".",
        config.packet_stuffing == ST_basic  ? "basic"
        : config.packet_stuffing == ST_soxr ? "soxr"
                                            : "auto");
  debug(1, "interpolation soxr_delay_threshold is %d.", config.soxr_delay_threshold);
  debug(1, "resync time is %f seconds.", config.resync_threshold);
  debug(1, "resync recovery time is %f seconds.", config.resync_recovery_time);
  debug(1, "allow a session to be interrupted: %d.", config.allow_session_interruption);
  debug(1, "busy timeout time is %d.", config.timeout);
  debug(1, "drift tolerance is %f seconds.", config.tolerance);
  debug(1, "password is \"%s\".", strnull(config.password));
  debug(1, "default airplay volume is: %.6f.", config.default_airplay_volume);
  debug(1, "high threshold airplay volume is: %.6f.", config.high_threshold_airplay_volume);
  if (config.limit_to_high_volume_threshold_time_in_minutes == 0)
    debug(1, "check for higher-than-threshold volume for new play session is disabled.");
  else
    debug(1,
          "suggest default airplay volume for new play sessions instead of higher-than-threshold "
          "airplay volume after: %d minutes.",
          config.limit_to_high_volume_threshold_time_in_minutes);
  debug(1, "ignore_volume_control is %d.", config.ignore_volume_control);
  if (config.volume_max_db_set)
    debug(1, "volume_max_db is %d.", config.volume_max_db);
  else
    debug(1, "volume_max_db is not set");
  debug(1, "volume range in dB (zero means use the range specified by the mixer): %u.",
        config.volume_range_db);
  debug(1,
        "volume_range_combined_hardware_priority (1 means hardware mixer attenuation is used "
        "first) is %d.",
        config.volume_range_hw_priority);
  debug(1, "playback_mode is %d (0-stereo, 1-mono, 1-reverse_stereo, 2-both_left, 3-both_right).",
        config.playback_mode);
  debug(1, "disable_synchronization is %d.", config.no_sync);
  debug(1, "use_mmap_if_available is %d.", config.no_mmap ? 0 : 1);
  debug(1, "output_format automatic selection is %sabled.",
        config.output_format_auto_requested ? "en" : "dis");
  if (config.output_format_auto_requested == 0)
    debug(1, "output_format is \"%s\".", sps_format_description_string(config.output_format));
  debug(1, "output_rate automatic selection is %sabled.",
        config.output_rate_auto_requested ? "en" : "dis");
  if (config.output_rate_auto_requested == 0)
    debug(1, "output_rate is %d.", config.output_rate);
  debug(1, "audio backend desired buffer length is %f seconds.",
        config.audio_backend_buffer_desired_length);
  debug(1, "audio_backend_buffer_interpolation_threshold_in_seconds is %f seconds.",
        config.audio_backend_buffer_interpolation_threshold_in_seconds);
  debug(1, "audio backend latency offset is %f seconds.", config.audio_backend_latency_offset);
  if (config.audio_backend_silent_lead_in_time_auto == 1)
    debug(1, "audio backend silence lead-in time is \"auto\".");
  else
    debug(1, "audio backend silence lead-in time is %f seconds.",
          config.audio_backend_silent_lead_in_time);
  debug(1, "zeroconf regtype is \"%s\".", config.regtype);
  debug(1, "decoders_supported field is %d.", config.decoders_supported);
  debug(1, "use_apple_decoder is %d.", config.use_apple_decoder);
  debug(1, "alsa_use_hardware_mute is %d.", config.alsa_use_hardware_mute);
  if (config.interface)
    debug(1, "mdns service interface \"%s\" requested.", config.interface);
  else
    debug(1, "no special mdns service interface was requested.");
  char *realConfigPath = realpath(config.configfile, NULL);
  if (realConfigPath) {
    debug(1, "configuration file name \"%s\" resolves to \"%s\".", config.configfile,
          realConfigPath);
    free(realConfigPath);
  } else {
    debug(1, "configuration file name \"%s\" can not be resolved.", config.configfile);
  }
#ifdef CONFIG_METADATA
  debug(1, "metadata enabled is %d.", config.metadata_enabled);
  debug(1, "metadata pipename is \"%s\".", config.metadata_pipename);
  debug(1, "metadata socket address is \"%s\" port %d.", config.metadata_sockaddr,
        config.metadata_sockport);
  debug(1, "metadata socket packet size is \"%d\".", config.metadata_sockmsglength);
  debug(1, "get-coverart is %d.", config.get_coverart);
#endif
#ifdef CONFIG_MQTT
  debug(1, "mqtt is %sabled.", config.mqtt_enabled ? "en" : "dis");
  debug(1, "mqtt hostname is %s, port is %d.", config.mqtt_hostname, config.mqtt_port);
  debug(1, "mqtt topic is %s.", config.mqtt_topic);
  debug(1, "mqtt will%s publish raw metadata.", config.mqtt_publish_raw ? "" : " not");
  debug(1, "mqtt will%s publish parsed metadata.", config.mqtt_publish_parsed ? "" : " not");
  debug(1, "mqtt will%s publish cover Art.", config.mqtt_publish_cover ? "" : " not");
  debug(1, "mqtt remote control is %sabled.", config.mqtt_enable_remote ? "en" : "dis");
#endif

#ifdef CONFIG_CONVOLUTION
  debug(1, "convolution is %d.", config.convolution);
  debug(1, "convolution IR file is \"%s\"", config.convolution_ir_file);
  debug(1, "convolution max length %d", config.convolution_max_length);
  debug(1, "convolution gain is %f", config.convolution_gain);
#endif
  debug(1, "loudness is %d.", config.loudness);
  debug(1, "loudness reference level is %f", config.loudness_reference_volume_db);

#ifdef CONFIG_SOXR
  pthread_create(&soxr_time_check_thread, NULL, &soxr_time_check, NULL);
  soxr_time_check_thread_started = 1;
#endif


  // In AirPlay 2 mode, the AP1 prefix is the same as the device ID less the colons
  // In AirPlay 1 mode, the AP1 prefix is calculated by hashing the service name.
#ifndef CONFIG_AIRPLAY_2

  uint8_t ap_md5[16];

  // debug(1, "size of hw_addr is %u.", sizeof(config.hw_addr));
#ifdef CONFIG_OPENSSL
  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, config.service_name, strlen(config.service_name));
  MD5_Update(&ctx, config.hw_addr, sizeof(config.hw_addr));
  MD5_Final(ap_md5, &ctx);
#endif

#ifdef CONFIG_MBEDTLS
#if MBEDTLS_VERSION_MINOR >= 7
  mbedtls_md5_context tctx;
  mbedtls_md5_starts_ret(&tctx);
  mbedtls_md5_update_ret(&tctx, (unsigned char *)config.service_name, strlen(config.service_name));
  mbedtls_md5_update_ret(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
  mbedtls_md5_finish_ret(&tctx, ap_md5);
#else
  mbedtls_md5_context tctx;
  mbedtls_md5_starts(&tctx);
  mbedtls_md5_update(&tctx, (unsigned char *)config.service_name, strlen(config.service_name));
  mbedtls_md5_update(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
  mbedtls_md5_finish(&tctx, ap_md5);
#endif
#endif

#ifdef CONFIG_POLARSSL
  md5_context tctx;
  md5_starts(&tctx);
  md5_update(&tctx, (unsigned char *)config.service_name, strlen(config.service_name));
  md5_update(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
  md5_finish(&tctx, ap_md5);
#endif

  memcpy(config.ap1_prefix, ap_md5, sizeof(config.ap1_prefix));
#endif

#ifdef CONFIG_METADATA
  metadata_init(); // create the metadata pipe if necessary
#endif

#ifdef CONFIG_METADATA_HUB
  // debug(1, "Initialising metadata hub");
  metadata_hub_init();
#endif

#ifdef CONFIG_DACP_CLIENT
  // debug(1, "Requesting DACP Monitor");
  dacp_monitor_start();
#endif

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
  // Start up DBUS services after initial settings are all made
  // debug(1, "Starting up D-Bus services");
  pthread_create(&dbus_thread, NULL, &dbus_thread_func, NULL);
#ifdef CONFIG_DBUS_INTERFACE
  start_dbus_service();
#endif
#ifdef CONFIG_MPRIS_INTERFACE
  start_mpris_service();
#endif
#endif

#ifdef CONFIG_MQTT
  if (config.mqtt_enabled) {
    initialise_mqtt();
  }
#endif

#ifdef CONFIG_AIRPLAY_2
  ptp_send_control_message_string("T"); // get nqptp to create the named shm interface
  int ptp_check_times = 0;
  const int ptp_wait_interval_us = 5000;
  // wait for up to ten seconds for NQPTP to come online
  do {
    ptp_send_control_message_string("T"); // get nqptp to create the named shm interface
    usleep(ptp_wait_interval_us);
    ptp_check_times++;
  } while ((ptp_shm_interface_open() != 0) &&
           (ptp_check_times < (10000000 / ptp_wait_interval_us)));

  if (ptp_shm_interface_open() != 0) {
    die("Can't access NQPTP! Is it installed and running?");
  } else {
    if (ptp_check_times == 1)
      debug(1, "NQPTP is online.");
    else
      debug(1, "NQPTP is online after %u microseconds.", ptp_check_times * ptp_wait_interval_us);
  }
#endif

#ifdef CONFIG_METADATA
  send_ssnc_metadata('svna', config.service_name, strlen(config.service_name), 1);
  char buffer[256] = "";
  snprintf(buffer, sizeof(buffer), "%d", config.output_rate);
  send_ssnc_metadata('ofps', buffer, strlen(buffer), 1);
  snprintf(buffer, sizeof(buffer), "%s", sps_format_description_string(config.output_format));
  send_ssnc_metadata('ofmt', buffer, strlen(buffer), 1);
#endif

  activity_monitor_start(); // not yet for AP2
  pthread_create(&rtsp_listener_thread, NULL, &rtsp_listen_loop, NULL);
  atexit(exit_rtsp_listener);
  pthread_join(rtsp_listener_thread, NULL);
  return 0;
}
