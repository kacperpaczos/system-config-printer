/* -*- Mode: C; c-file-style: "gnu" -*-
 * udev-configure-printer - a udev callout to configure print queues
 * Copyright (C) 2009 Red Hat, Inc.
 * Author: Tim Waugh <twaugh@redhat.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 * The protocol for this program is that it is called by udev with
 * these arguments:
 *
 * 1. "add" or "remove"
 * 2. For "add":    the path (%p) of the device
 *    For "remove": the CUPS device URI corresponding to the queue
 *
 * For "add", it will output the following to stdout:
 *
 * REMOVE_CMD="$0 remove $DEVICE_URI"
 *
 * where $0 is argv[0] and $DEVICE_URI is the CUPS device URI
 * corresponding to the queue.
 */

#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE 1

#include <cups/cups.h>
#include <cups/http.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

struct device_id
{
  char *full_device_id;
  char *mfg;
  char *mdl;
  char *sern;
};

static void
free_device_id (struct device_id *id)
{
  free (id->full_device_id);
  free (id->mfg);
  free (id->mdl);
  free (id->sern);
}

static void
parse_device_id (const char *device_id,
		 struct device_id *id)
{
  char *fieldname;
  char *start, *end;
  size_t len;

  id->full_device_id = strdup (device_id);
  fieldname = strdup (device_id);
  if (id->full_device_id == NULL || fieldname == NULL)
    {
      syslog (LOG_ERR, "out of memory");
      exit (1);
    }

  fieldname[0] = '\0';

  start = id->full_device_id;
  while (*start != '\0')
    {
      /* New field. */

      end = start;
      while (*end != '\0' && *end != ':')
	end++;

      if (*end == '\0')
	break;

      len = end - start;
      memcpy (fieldname, start, len);
      fieldname[len] = '\0';

      start = end + 1;
      while (*end != '\0' && *end != ';')
	end++;

      len = end - start;

      if (!id->mfg &&
	  !strncasecmp (fieldname, "MANUFACTURER", 12) ||
	  !strncasecmp (fieldname, "MFG", 3))
	id->mfg = strndup (start, len);
      else if (!id->mdl &&
	       !strncasecmp (fieldname, "MODEL", 5) ||
	       !strncasecmp (fieldname, "MDL", 3))
	id->mdl = strndup (start, len);
      else if (!id->sern &&
	       !strncasecmp (fieldname, "SERIALNUMBER", 12) ||
	       !strncasecmp (fieldname, "SERN", 4) ||
	       !strncasecmp (fieldname, "SN", 2))
	id->sern = strndup (start, len);

      if (*end != '\0')
	start = end + 1;
    }

  free (fieldname);
}

static void
device_id_from_devpath (const char *devpath,
			struct device_id *id)
{
  struct udev *udev;
  struct udev_device *dev, *dev_iface;
  const char *sys;
  size_t syslen, devpathlen;
  char *syspath;
  const char *ieee1284_id;

  id->full_device_id = id->mfg = id->mdl = id->sern = NULL;

  udev = udev_new ();
  if (udev == NULL)
    {
      syslog (LOG_ERR, "udev_new failed");
      exit (1);
    }

  sys = udev_get_sys_path (udev);
  syslen = strlen (sys);
  devpathlen = strlen (devpath);
  syspath = malloc (syslen + devpathlen + 1);
  if (syspath == NULL)
    {
      udev_unref (udev);
      syslog (LOG_ERR, "out of memory");
      exit (1);
    }

  memcpy (syspath, sys, syslen);
  memcpy (syspath + syslen, devpath, devpathlen);
  syspath[syslen + devpathlen] = '\0';

  dev = udev_device_new_from_syspath (udev, syspath);
  if (dev == NULL)
    {
      udev_device_unref (dev);
      udev_unref (udev);
      syslog (LOG_ERR, "unable to access %s", syspath);
      exit (1);
    }

#if 0
  dev_iface = udev_device_get_parent_with_subsystem_devtype (dev, "usb",
							     "usb_interface");
  if (dev_iface == NULL)
    {
      udev_device_unref (dev);
      udev_unref (udev);
      syslog (LOG_ERR, "unable to access usb_interface device of %s",
	      syspath);
      exit (1);
    }

  ieee1284_id = udev_device_get_sysattr_value (dev_iface, "ieee1284_id");
#endif
  ieee1284_id = "MFG:EPSON;CMD:ESCPL2,BDC,D4,D4PX;MDL:Stylus D78;CLS:PRINTER;DES:EPSON Stylus D78;";
  if (ieee1284_id != NULL)
    {
      syslog (LOG_DEBUG, "ieee1284_id=%s", ieee1284_id);
      parse_device_id (ieee1284_id, id);
    }

  udev_device_unref (dev);
}

static const char *
no_password (const char *prompt)
{
  return "";
}

static char *
find_matching_device_uri (struct device_id *id)
{
  int tries = 6;
  http_t *cups = NULL;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  const char *include_schemes[] = { "usb" };
  char *device_uri;

  cupsSetPasswordCB (no_password);
  while (cups == NULL && tries-- > 0)
    {
      cups = httpConnectEncrypt ("localhost", 631,
				 HTTP_ENCRYPT_IF_REQUESTED);
      if (cups)
	break;

      syslog (LOG_DEBUG, "failed to connect to CUPS server; retrying in 5s");
      sleep (5);
    }

  if (cups == NULL)
    {
      syslog (LOG_DEBUG, "failed to connect to CUPS server; giving up");
      exit (1);
    }

  request = ippNewRequest (CUPS_GET_DEVICES);
  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_NAME, "include-schemes",
		 sizeof (include_schemes) / sizeof(include_schemes[0]),
		 NULL, include_schemes);

  answer = cupsDoRequest (cups, request, "/");
  httpClose (cups);
  if (answer == NULL)
    {
      syslog (LOG_ERR, "failed to send CUPS-Get-Devices request");
      exit (1);
    }

  if (answer->request.status.status_code > IPP_OK_CONFLICT)
    {
      syslog (LOG_ERR, "CUPS-Get-Devices request failed (%d)",
	      answer->request.status.status_code);
      exit (1);
    }

  for (attr = answer->attrs; attr; attr = attr->next)
    {
      while (attr && attr->group_tag != IPP_TAG_PRINTER)
	attr = attr->next;

      if (!attr)
	break;

      for (; attr && attr->group_tag == IPP_TAG_PRINTER; attr = attr->next)
	{
	  if (!strcmp (attr->name, "device-uri") &&
	      attr->value_tag == IPP_TAG_URI)
	    {
	      char scheme[HTTP_MAX_URI];
	      char username[HTTP_MAX_URI];
	      char mfg[HTTP_MAX_URI];
	      char resource[HTTP_MAX_URI];
	      int port;
	      char *mdl;
	      char *serial;
	      size_t seriallen, mdllen = 0;
	      syslog (LOG_DEBUG, "uri:%s", attr->values[0].string.text);
	      httpSeparateURI (HTTP_URI_CODING_ALL,
			       attr->values[0].string.text,
			       scheme, sizeof(scheme),
			       username, sizeof(username),
			       mfg, sizeof(mfg),
			       &port,
			       resource, sizeof(resource));

	      mdl = resource;
	      if (*mdl == '/')
		mdl++;

	      serial = strstr (mdl, "?serial=");
	      if (serial)
		{
		  mdllen = serial - mdl;
		  serial += 8;
		  seriallen = strspn (serial, "&");
		}

	      syslog (LOG_DEBUG, "%s <=> %s", mfg, id->mfg);
	      if (strcasecmp (mfg, id->mfg))
		continue;

	      syslog (LOG_DEBUG, "%s <=> %s (%d)", mdl, id->mdl, mdllen);
	      if (mdllen)
		{
		  if (strncasecmp (mdl, id->mdl, mdllen))
		    continue;
		}
	      else if (strcasecmp (mdl, id->mdl))
		continue;

	      if (serial)
		{
		  if (id->sern)
		    {
		      if (!strcasecmp (serial, id->sern))
			{
			  /* Serial number matches so stop looking. */
			  device_uri = strdup (attr->values[0].string.text);
			  break;
			}
		      else
			continue;
		    }
		  else
		    continue;
		}

	      device_uri = strdup (attr->values[0].string.text);
	    }
	}

      if (!attr)
	break;
    }

  ippDelete (answer);
  return device_uri;
}

static int
do_add (const char *cmd, const char *devpath)
{
  struct device_id id;
  char *device_uri = NULL;
  int i;

  syslog (LOG_DEBUG, "add %s", devpath);

  device_id_from_devpath (devpath, &id);
  if (!id.mfg || !id.mdl)
    {
      syslog (LOG_ERR, "invalid IEEE 1284 Device ID");
      exit (1);
    }

  syslog (LOG_DEBUG, "MFG:%s MDL:%s SERN:%s", id.mfg, id.mdl,
	  id.sern ? id.sern : "-");

  /* If the manufacturer's name appears as the start of the model
     name, remove it. */
  i = 0;
  while (id.mfg[i] != '\0')
    if (id.mfg[i] != id.mdl[i])
      break;
  if (id.mfg[i] == '\0')
    {
      char *old = id.mdl;
      id.mdl = strdup (id.mdl + i);
      free (old);
    }

  syslog (LOG_DEBUG, "Match MFG:%s MDL:%s SERN:%s", id.mfg, id.mdl,
	  id.sern ? id.sern : "-");

  device_uri = find_matching_device_uri (&id);
  free_device_id (&id);

  syslog (LOG_DEBUG, "Device URI: %s", device_uri ? device_uri : "?");

  if (device_uri)
    {
      printf ("REMOVE_CMD=\"%s remove %s\"\n", cmd, device_uri);
      free (device_uri);
    }

  return 0;
}

static int
do_remove (const char *uri)
{
  syslog (LOG_DEBUG, "remove %s", uri);
  return 0;
}

int
main (int argc, char **argv)
{
  int add;

  openlog ("udev-configure-printer", 0, LOG_LPR);
  if (argc != 3 ||
      !((add = !strcmp (argv[1], "add")) ||
	!strcmp (argv[1], "remove")))
    {
      fprintf (stderr,
	       "Syntax: %s add {USB device path}\n"
	       "        %s remove {CUPS device URI}\n",
	       argv[0], argv[0]);
      return 1;
    }

  if (add)
    return do_add (argv[0], argv[2]);

  return do_remove (argv[2]);
}
