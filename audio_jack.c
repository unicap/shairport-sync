/*
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

/*
 * jack output driver for shairport
 * Copyright (c) Arne Caspari 2015 <arne@unicap-imaging.org>
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <stdlib.h>
#include <errno.h>
#include <jack/jack.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <soxr.h>
#include "common.h"
#include "audio.h"

static char *portspec = NULL;
static jack_port_t *output_port1, *output_port2;
static jack_client_t *client = NULL;
static int source_samplerate = 0;
static int target_samplerate = 0;
static float *output_buffer_1 = NULL;
static float *output_buffer_2 = NULL;
static int output_writep;
static int output_readp;
static int output_buffer_size = 0;

#define CLIENT_NAME "shairport"
#define BUFFER_SIZE_FACTOR 1


static int
output_jack_process (jack_nframes_t nframes, void *arg);


static void start(int sample_rate) {
	const char **ports;
	jack_status_t status;
	int i;

	if (output_buffer_1)
		free (output_buffer_1);

	if (output_buffer_2)
		free (output_buffer_2);

	source_samplerate = sample_rate;

	output_buffer_size = sample_rate * BUFFER_SIZE_FACTOR;
	output_buffer_1 = malloc (sizeof(float) * output_buffer_size);
	output_buffer_2 = malloc (sizeof(float) * output_buffer_size);
	output_writep = output_readp = 0;

	if (client)
		return;

	client = jack_client_open (CLIENT_NAME, JackNullOption, &status, NULL);
	if (client == NULL) {
		fprintf (stderr, "jack_client_open() failed, "
			 "status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return;
	}

	if (status & JackServerStarted){
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		const char *client_name;
		client_name = jack_get_client_name(client);
		fprintf (stderr, "unique name `%s' assigned\n", client_name);
	}

	/* create two ports */
	output_port1 = jack_port_register (client, "output_1",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	output_port2 = jack_port_register (client, "output_2",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	if ((output_port1 == NULL) || (output_port2 == NULL)) {
		fprintf(stderr, "no more JACK ports available\n");
		exit (1);
	}

	target_samplerate = jack_get_sample_rate (client);

	jack_set_process_callback (client, output_jack_process, NULL);

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */
	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
		exit (1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */

	ports = jack_get_ports (client, portspec, NULL,
				JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no playback ports match portspec \"%s\"\n",
			portspec);
		exit (1);
	}

	if (jack_connect (client, jack_port_name (output_port1), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	if (jack_connect (client, jack_port_name (output_port2), ports[1])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	jack_free (ports);
}

int
output_jack_process (jack_nframes_t n_out_frames, void *arg)
{
	jack_default_audio_sample_t *jack_out1, *jack_out2;
	float *out1, *out2;
	size_t nframes = (size_t)(n_out_frames *
				  source_samplerate / target_samplerate + 0.5);

	jack_out1 = (jack_default_audio_sample_t*)
		jack_port_get_buffer (output_port1, nframes);
	jack_out2 = (jack_default_audio_sample_t*)
		jack_port_get_buffer (output_port2, nframes);

	if ((output_readp + nframes) >= output_buffer_size){
		int frames_to_copy = output_buffer_size - output_readp;
		int remain = nframes - frames_to_copy;

		if ((output_writep > output_readp) ||
		    (output_writep < remain)){
			/* printf ("buffer underrun1! %d : %d : %d\n", */
			/* 	output_readp, output_readp + nframes, */
			/* 	output_writep); */
			return 0;
		}
	} else if ((output_readp < output_writep) &&
		   (output_readp + nframes) > (output_writep)){
		/* printf ("buffer underrun2! %d > %d\n", */
		/* 	output_readp + nframes, output_writep); */
		return 0;
	}

	out1 = malloc (nframes * sizeof(float));
	out2 = malloc (nframes * sizeof(float));
	if ((output_readp + nframes) > output_buffer_size ){
		int frames_to_copy = output_buffer_size - output_readp;
		int remain = nframes - frames_to_copy;
		memcpy (out1, output_buffer_1 + output_readp,
			frames_to_copy * sizeof (float));
		memcpy (out1 + frames_to_copy, output_buffer_1,
			remain * sizeof (float));
		memcpy (out2, output_buffer_2 + output_readp,
			frames_to_copy * sizeof (float));
		memcpy (out2 + frames_to_copy, output_buffer_2,
			remain * sizeof (float));
		output_readp = remain;
	} else {
		memcpy (out1, output_buffer_1 + output_readp,
			nframes * sizeof (float));
		memcpy (out2, output_buffer_2 + output_readp,
			nframes * sizeof (float));
		output_readp += nframes;
		if (output_readp == output_buffer_size){
			output_readp = 0;
		}
	}

	size_t odone;

	soxr_error_t error = soxr_oneshot (source_samplerate,
					   target_samplerate,
					   1,
					   out1, nframes, NULL,
					   jack_out1, n_out_frames, &odone,
					   NULL, NULL, NULL);
	error = soxr_oneshot (source_samplerate,
			      target_samplerate,
			      1,
			      out2, nframes, NULL,
			      jack_out2, n_out_frames, &odone,
			      NULL, NULL, NULL);

	free (out1);
	free (out2);

	return 0;
}

static void play(short buf[], int samples) {
	int frames_to_copy = 0;
	int remain = 0;

	int i,j;

	/* printf ("got %d samples\n", samples); */
	if ((output_writep + samples) >= output_buffer_size){
		int frames_to_copy = output_buffer_size - output_writep;
		int remain = samples - frames_to_copy;

		if ((output_readp > output_writep) ||
		    (output_readp < remain)){
			printf ("buffer overrun1! %d : %d : %d\n", output_readp, output_readp + samples, output_writep);
			return;
		}
	} else if ((output_writep < output_readp) &&
		   (output_writep + samples) > (output_readp)){
		printf ("buffer overrun2! %d > %d\n", output_readp + samples, output_writep);
		return;
	}

	if ((output_writep + samples) > output_buffer_size){
		frames_to_copy = output_buffer_size - output_writep;
		remain = samples - frames_to_copy;
	} else {
		frames_to_copy = samples;
		remain = 0;
	}

	for( i=output_writep, j=0; i < output_writep+frames_to_copy; i++, j++ )
	{
		output_buffer_1[i] = (float)((float)buf[j*2]/(float)SHRT_MAX);  /* left */
		output_buffer_2[i] = (float)((float)buf[j*2+1]/(float)SHRT_MAX);  /* right */
	}
	for( i=0; i < remain; i++, j++ )
	{
		output_buffer_1[i] = (float)((float)buf[j*2]/(float)SHRT_MAX);  /* left */
		output_buffer_2[i] = (float)((float)buf[j*2+1]/(float)SHRT_MAX);  /* right */
	}

	if (remain != 0){
		output_writep = remain;
	} else {
		output_writep += samples;
		if (output_writep == output_buffer_size)
			output_writep = 0;
	}
}

static void stop(void) {
	printf ("STOP\n");
}

static int init(int argc, char **argv) {
	debug(1, "jack init");
	const char *str;
	int value;

	config.audio_backend_buffer_desired_length = 44100; // one second.
	config.audio_backend_latency_offset = 0;

	if (config.cfg != NULL) {
		/* Get the Output Pipename. */
		const char *str;
		if (config_lookup_string(config.cfg, "jack.portname", &str)) {
			portspec = (char *)str;
		}

		/* Get the desired buffer size setting. */
		if (config_lookup_int(config.cfg, "jack.audio_backend_buffer_desired_length", &value)) {
			if ((value < 0) || (value > 132300))
				die("Invalid jack audio backend buffer desired length \"%d\". It should be between 0 and 132300, default is 44100",
				    value);
			else
				config.audio_backend_buffer_desired_length = value;
		}

		/* Get the latency offset. */
		if (config_lookup_int(config.cfg, "jack.audio_backend_latency_offset", &value)) {
			if ((value < -66150) || (value > 66150))
				die("Invalid pipe audio backend buffer latency offset \"%d\". It should be between -66150 and +66150, default is 0",
				    value);
			else
				config.audio_backend_latency_offset = value;
		}
	}

	if (argc == 1)
		portspec = strdup(argv[0]);

	debug(1, "portspec is \"%s\"", portspec);

	return 0;
}

static void deinit(void) {
	if (client)
		jack_client_close (client);
}

static void help(void) {
  printf("    jack takes 1 argument: the name of the port to connect to.\n");
}

audio_output audio_jack = {.name = "jack",
                           .help = &help,
                           .init = &init,
                           .deinit = &deinit,
                           .start = &start,
                           .stop = &stop,
                           .flush = NULL,
                           .delay = NULL,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL};
