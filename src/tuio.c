/*
 * Copyright (c) 2009 Ryan Huffman
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Authors:
 *	Ryan Huffman (ryanhuffman@gmail.com)
 *	Andreas Willich (sabotageandi@gmail.com)
 *	Brady O'Brien (baobrien@evtron.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>

#include <xf86Xinput.h>
#include <xf86_OSlib.h>
#include <xserver-properties.h>
#include <xf86Module.h>

#include "tuio.h"

/* InputInfoPtr for main tuio device */
static InputInfoPtr g_pInfo;
static int pipefd[2] = {-1, -1};

/* Module Functions */
static pointer
TuioPlug(pointer, pointer, int *, int *);

static void
TuioUnplug(pointer);

/* Driver Functions */
static int
TuioPreInit(InputDriverPtr,InputInfoPtr , int);

static void
TuioUnInit(InputDriverPtr, InputInfoPtr, int);

static void
TuioObjReadInput(InputInfoPtr pInfo);

static void
TuioReadInput(InputInfoPtr);

static int
TuioControl(DeviceIntPtr, int);



static int
_init_buttons(DeviceIntPtr device);

static int
_init_axes(DeviceIntPtr device);

static int
_tuio_lo_2dcur_handle(const char *path,
                   const char *types,
                   lo_arg **argv,
                   int argc,
                   void *data,
                   void *user_data);

static void
_free_tuiodev(TuioDevicePtr pTuio);

static void
_lo_error(int num,
         const char *msg,
         const char *path);

/* Object and Subdev list manipulation functions */
static ObjectPtr
_object_get(ObjectPtr head, int id);

static ObjectPtr 
_object_new(int id);

static void
_object_add(ObjectPtr *obj_list, ObjectPtr obj);

static ObjectPtr
_object_remove(ObjectPtr *obj_list, int id);

static void
_subdev_add(InputInfoPtr pInfo, SubDevicePtr subdev);

static void
_subdev_remove(InputInfoPtr pInfo, InputInfoPtr sub_pInfo);

static SubDevicePtr
_subdev_get(InputInfoPtr pInfo, SubDevicePtr *subdev_list);



/* Driver information */
static XF86ModuleVersionInfo TuioVersionRec =
{
    "tuio_mt",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT InputDriverRec TUIO = 
{
    1,
    "tuio_mt",
    NULL,
    TuioPreInit,
    TuioUnInit,
    NULL,
    0
};

_X_EXPORT XF86ModuleData tuioModuleData =
{
    &TuioVersionRec,
    TuioPlug,
    TuioUnplug
};

static pointer
TuioPlug(pointer	module,
         pointer	options,
         int		*errmaj,
         int		*errmin)
{
    xf86AddInputDriver(&TUIO, module, 0);
    return module;
}

/**
 * Unplug
 */
static void
TuioUnplug(pointer	p)
{
}

/**
 * Pre-initialization of new device
 * This can be entered by either a "core" tuio device
 * or an extension "Object" device that is used for routing individual object
 * events through.
 */
static int
TuioPreInit(InputDriverPtr drv,
            InputInfoPtr    pInfo,//IDevPtr dev,
            int flags)
{
    //InputInfoPtr  pInfo;
    TuioDevicePtr pTuio = NULL;
    ObjectPtr obj;
    char *type;
    int num_subdev, tuio_port;

    //if (!(pInfo = xf86AllocateInput(drv, 0)))
     //   return NULL;

    /* If Type == Object, this is a device for an object to use */
    /*type = xf86CheckStrOption(dev->commonOptions, "Type", NULL); 

    if (type != NULL && strcmp(type, "Object") == 0) {
        xf86Msg(X_INFO, "%s: TUIO subdevice found\n", dev->identifier);

    } else {
*/
        if (!(pTuio = malloc(sizeof(TuioDeviceRec)))) {
            xf86DeleteInput(pInfo, 0);
            return NULL;
        }
        g_pInfo = pInfo;

        pInfo->private = pTuio;

        pTuio->num_subdev = 0;

        /* Get the number of subdevices we need to create */
        num_subdev = xf86SetIntOption(pInfo->options, "SubDevices",
                DEFAULT_SUBDEVICES); 
        if (num_subdev > MAX_SUBDEVICES) {
            num_subdev = MAX_SUBDEVICES;
        } else if (num_subdev < MIN_SUBDEVICES) {
            num_subdev = MIN_SUBDEVICES;
        }
        pTuio->init_num_subdev = num_subdev;

        /* Get the TUIO port number to use */
        tuio_port = xf86SetIntOption(pInfo->options, "Port", DEFAULT_PORT);
        if (tuio_port < 0 || tuio_port > 65535) {
            xf86Msg(X_INFO, "%s: Invalid port number (%i), defaulting to %i\n",
                    pInfo->name, tuio_port, DEFAULT_PORT);
            tuio_port = DEFAULT_PORT;
        }
        xf86Msg(X_INFO, "%s: TUIO UDP Port set to %i\n",
                pInfo->name, tuio_port);
        pTuio->tuio_port = tuio_port;

        /* Get setting for checking fseq numbers in TUIO packets */
        pTuio->fseq_threshold= xf86SetIntOption(pInfo->options,
                "FseqThreshold", DEFAULT_FSEQ_THRESHOLD);
        if (pTuio->fseq_threshold < 0) {
            pTuio->fseq_threshold = 0;
        }
        xf86Msg(X_INFO, "%s: FseqThreshold set to %i\n",
                pInfo->name, pTuio->fseq_threshold);

        /* Get setting for whether to send button events or not with
         * object add & remove */
        pTuio->post_button_events = xf86SetBoolOption(pInfo->options,
                "PostButtonEvents", True);

        /* Get setting for whether to hide devices when idle */
        pTuio->hide_devices = xf86SetBoolOption(pInfo->options,
                "PseudoHide", True);
    //}

    /* Set up InputInfoPtr */
    //(pInfo->name = xstrdup(dev->identifier);
    pInfo->flags = 0;
    pInfo->type_name = strdup(XI_TOUCHSCREEN); /* FIXME: Correct type? */ /* Now it is */
    //pInfo->conf_idev = dev;
    pInfo->read_input = pTuio ? TuioReadInput : TuioObjReadInput; /* Set callback */
    pInfo->device_control = TuioControl; /* Set callback */
    pInfo->switch_mode = NULL;
    
    /* Process common device options */
    xf86CollectInputOptions(pInfo, NULL);
    xf86ProcessCommonOptions(pInfo, pInfo->options);

    //pInfo->flags |= XI86_OPEN_ON_INIT;
    //pInfo->flags |= XI86_CONFIGURED;

}

/**
 * Clean up
 */
static void
TuioUnInit(InputDriverPtr drv,
           InputInfoPtr pInfo,
           int flags)
{
    xf86DeleteInput(pInfo, 0);
}

/**
 * Empty callback for object device.
 */
static void
TuioObjReadInput(InputInfoPtr pInfo) {
    return;
}

/**
 * Handle new TUIO  data on the socket
 */
static void
TuioReadInput(InputInfoPtr pInfo)
{
    TuioDevicePtr pTuio = pInfo->private;
    ObjectPtr *obj_list = &pTuio->obj_list;
    ObjectPtr obj = pTuio->obj_list;
    SubDevicePtr *subdev_list = &pTuio->subdev_list;
    ObjectPtr objtmp;
    int valuators[NUM_VALUATORS];
    
    ValuatorMask * vmask = valuator_mask_new(NUM_VALUATORS);
    while (xf86WaitForInput(pInfo->fd, 0) > 0)
    {
        /* The liblo handler will set this flag if anything was processed */
        pTuio->processed = 0;

        /* liblo will receive a message and call the appropriate
         * handlers (i.e. _tuio_lo_cur2d_hande()) */
        lo_server_recv_noblock(pTuio->server, 0);

        /* During the processing of the previous message/bundle,
         * any "active" messages will be handled by flagging
         * the listed object ids.  Now that processing is done,
         * remove any dead object ids and set any pending changes.
         * Also check to make sure the processed data was newer than
         * the last processed data */
        if (pTuio->processed &&
            (pTuio->fseq_new > pTuio->fseq_old ||
             pTuio->fseq_old - pTuio->fseq_new > pTuio->fseq_threshold)) {

            while (obj != NULL) {
                if (!obj->alive) {
                    if (obj->subdev && pTuio->post_button_events) {
                        /* Post button "up" event */
                        xf86PostButtonEvent(obj->subdev->pInfo->dev, TRUE, 1, FALSE, 0, 0);
                    }
                    valuators[0] = obj->xpos * 0x7FFFFFFF;
                    valuators[1] = obj->ypos * 0x7FFFFFFF;
                    valuators[2] = obj->xvel * 0x7FFFFFFF;
                    valuators[3] = obj->yvel * 0x7FFFFFFF;
                    valuator_mask_set_range(vmask,0,NUM_VALUATORS,valuators);

                    xf86PostTouchEvent(pInfo->dev,obj->id,XI_TouchEnd,0,vmask);

                    //xf86PostMotionEventP(obj->subdev->pInfo->dev,
                    //        TRUE, /* is_absolute */
                    //        0, /* first_valuator */
                    //        NUM_VALUATORS, /* num_valuators */
                    //       valuators);

                    objtmp = obj->next;
                    obj = _object_remove(obj_list, obj->id);
                    _subdev_add(pInfo, obj->subdev);
                    free(obj);
                    obj = objtmp;


                } else {
                    /* Object is alive.  Check to see if an update has been set.
                     * If it has been updated and it has a subdevice to send
                     * events on, send the event) */
                    if (obj->pending.set && obj->subdev) {
                        obj->xpos = obj->pending.xpos;
                        obj->ypos = obj->pending.ypos;
                        obj->xvel = obj->pending.xvel;
                        obj->yvel = obj->pending.yvel;
                        obj->pending.set = False;

                        /* OKAY FOR NOW, maybe update with a better range? */
                        /* TODO: Add more valuators with additional information */
                        valuators[0] = obj->xpos * 0x7FFFFFFF;
                        valuators[1] = obj->ypos * 0x7FFFFFFF;
                        valuators[2] = obj->xvel * 0x7FFFFFFF;
                        valuators[3] = obj->yvel * 0x7FFFFFFF;
			valuator_mask_set_range(vmask,0,NUM_VALUATORS,valuators);
			
                        //(xf86PostMotionEventP(obj->subdev->pInfo->dev,
                        //        TRUE, /* is_absolute */
                        //        0, /* first_valuator */
                        //        NUM_VALUATORS, /* num_valuators */
                        //        valuators);
                        
			//Object is new to screen and should be added
                        if (obj->pending.button) {
                          //  xf86PostButtonEvent(obj->subdev->pInfo->dev, TRUE, 1, TRUE, 0, 0);
				xf86PostTouchEvent(pInfo->dev,obj->id,XI_TouchBegin,0,vmask);
                            obj->pending.button = False;
                        }else {
				xf86PostTouchEvent(pInfo->dev,obj->id,XI_TouchUpdate,0,vmask);
			}

                    }
                    obj->alive = 0; /* Reset for next message */
                    obj = obj->next;
                }
            }
            pTuio->fseq_old = pTuio->fseq_new;
        }
    }
    valuator_mask_free(&vmask);
}

/**
 * Handle device state changes
 */
static int
TuioControl(DeviceIntPtr device,
            int what)
{
    InputInfoPtr pInfo = device->public.devicePrivate;
    TuioDevicePtr pTuio = pInfo->private;
    SubDevicePtr subdev;
    char *tuio_port;
    int res;

    switch (what)
    {
        case DEVICE_INIT:
            xf86Msg(X_INFO, "%s: Init\n", pInfo->name);
            _init_buttons(device);
            _init_axes(device);

            /* If this is a "core" device, create object devices */
            if (pTuio) {
               // _hal_create_devices(pInfo, pTuio->init_num_subdev); //TODO:Kill HAL completely
            }
            break;

        case DEVICE_ON: /* Open socket and start listening! */
            xf86Msg(X_INFO, "%s: On.\n", pInfo->name);
            if (device->public.on)
                break;

            /* If this is an object device, use a dummy pipe,
             * and add device to subdev list */
            if (!pTuio) {
                if (pipefd[0] == -1) {
                    SYSCALL(res = pipe(pipefd));
                    if (res == -1) {
                        xf86Msg(X_ERROR, "%s: failed to open pipe\n",
                                pInfo->name);
                        return BadAlloc;
                    }
                }

                pInfo->fd = pipefd[0];

                goto finish;
            }

            /* Setup server */
            asprintf(&tuio_port, "%i", pTuio->tuio_port);
            pTuio->server = lo_server_new_with_proto(tuio_port, LO_UDP, _lo_error);
            if (pTuio->server == NULL) {
                xf86Msg(X_ERROR, "%s: Error allocating new lo_server\n", 
                        pInfo->name);
                return BadAlloc;
            }

            /* Register to receive all /tuio/2Dcur messages */
            lo_server_add_method(pTuio->server, "/tuio/2Dcur", NULL, 
                                 _tuio_lo_2dcur_handle, pInfo);

            pInfo->fd = lo_server_get_socket_fd(pTuio->server);

            xf86FlushInput(pInfo->fd);

finish:     xf86AddEnabledDevice(pInfo);
            device->public.on = TRUE;

            /* Allocate device storage and add to device list */
            subdev = malloc(sizeof(SubDeviceRec));
            subdev->pInfo = pInfo;
            _subdev_add(g_pInfo, subdev);
            break;

        case DEVICE_OFF:
            xf86Msg(X_INFO, "%s: Off\n", pInfo->name);
            if (!device->public.on)
                break;

            xf86RemoveEnabledDevice(pInfo);

            if (pTuio) {
                lo_server_free(pTuio->server);
                pInfo->fd = -1;
            }
            /* Remove subdev from list - This applies for both subdevices
             * and the "core" device */
            _subdev_remove(g_pInfo, pInfo);

            device->public.on = FALSE;
            break;

        case DEVICE_CLOSE:
            xf86Msg(X_INFO, "%s: Close\n", pInfo->name);
            //_hal_remove_device(pInfo); TODO: hal bad
            break;

    }
    return Success;
}

/**
 * Initialize the device properties
 * TODO
 */
TuioPropertyInit() {
}

/**
 * Free a TuioDeviceRec
 */
static void
_free_tuiodev(TuioDevicePtr pTuio) {
    ObjectPtr obj = pTuio->obj_list;
    ObjectPtr tmp;

    while (obj != NULL) {
        tmp = obj->next;
        free(obj);
        obj = tmp;
    }

    free(pTuio);
}

/**
 * Handles OSC messages in the /tuio/2Dcur address space
 */
static int
_tuio_lo_2dcur_handle(const char *path,
                      const char *types,
                      lo_arg **argv,
                      int argc,
                      void *data,
                      void *user_data) {
    InputInfoPtr pInfo = user_data;
    TuioDevicePtr pTuio = pInfo->private;
    ObjectPtr *obj_list = &pTuio->obj_list;
    ObjectPtr obj, objtemp;
    char *act;
    int i;

    if (argc == 0) {
        xf86Msg(X_ERROR, "%s: Error in /tuio/cur2d (argc == 0)\n", 
                pInfo->name);
        return 0;
    } else if(*types != 's') {
        xf86Msg(X_ERROR, "%s: Error in /tuio/cur2d (types[0] != 's')\n", 
                pInfo->name);
        return 0;
    }

    /* Flag as being processed, used in TuioReadInput() */
    pTuio->processed = 1;

    /* Parse message type */
    /* Set message type:  */
    if (strcmp((char *)argv[0], "set") == 0) {

        /* Simple type check */
        if (strcmp(types, "sifffff")) {
            xf86Msg(X_ERROR, "%s: Error in /tuio/cur2d set msg (types == %s)\n", 
                    pInfo->name, types);
            return 0;
        }

        obj = _object_get(*obj_list, argv[1]->i);

        /* If not found, create a new object */
        if (obj == NULL) {
            obj = _object_new(argv[1]->i);
            _object_add(obj_list, obj);
            obj->subdev = _subdev_get(pInfo, &pTuio->subdev_list);
            if (obj->subdev && pTuio->post_button_events)
                obj->pending.button = True;
        }

        obj->pending.xpos = argv[2]->f;
        obj->pending.ypos = argv[3]->f;
        obj->pending.xvel = argv[4]->f;
        obj->pending.yvel = argv[5]->f;
        obj->pending.set = True;

    } else if (strcmp((char *)argv[0], "alive") == 0) {
        /* Mark all objects that are still alive */
        obj = *obj_list;
        while (obj != NULL) {
            for (i=1; i<argc; i++) {
                if (argv[i]->i == obj->id) {
                    obj->alive = True;
                    break;
                }
            }
            obj = obj->next;
        }

    } else if (strcmp((char *)argv[0], "fseq") == 0) {
        /* Simple type check */
        if (strcmp(types, "si")) {
            xf86Msg(X_ERROR, "%s: Error in /tuio/cur2d fseq msg (types == %s)\n", 
                    pInfo->name, types);
            return 0;
        }
        pTuio->fseq_new = argv[1]->i;

    }
    return 0;
}

/**
 * liblo error handler
 */
static void
_lo_error(int num,
         const char *msg,
         const char *path)
{
    xf86Msg(X_ERROR, "liblo: %s\n", msg);
}

/**
 * Retrieves an object from a list based on its id.
 *
 * @return NULL if not found.
 */
static ObjectPtr
_object_get(ObjectPtr head, int id) {
    ObjectPtr obj = head;

    while (obj != NULL && obj->id != id) {
        obj = obj->next;
    }

    return obj;
}

/**
 * Allocates and inserts a new object at the beginning of a list.
 * Pointer to the new object is returned.
 * Doesn't check for duplicate ids, so call _object_get() beforehand
 * to make sure it doesn't exist already!!
 *
 * @return ptr to newly inserted object
 */
static ObjectPtr 
_object_new(int id) {
    ObjectPtr new_obj = malloc(sizeof(ObjectRec));

    new_obj->id = id;
    new_obj->alive = True;

    return new_obj;
}

/**
 * Adds a SubDevice to the beginning of the subdev_list list
 */
static void
_object_add(ObjectPtr *obj_list, ObjectPtr obj) {

    if (obj_list == NULL || obj == NULL)
        return;

    if (*obj_list != NULL) {
        obj->next = *obj_list;
    }
    *obj_list = obj;
}


/**
 * Removes an Object with a specific id from a list.
 */
static ObjectPtr
_object_remove(ObjectPtr *obj_list, int id) {
    ObjectPtr obj = *obj_list;
    ObjectPtr objtmp;

    if (obj == NULL) return; /* Empty list */

    if (obj->id == id) { /* Remove from head */
        *obj_list = obj->next;
    } else {
        while (obj->next != NULL) {
            if (obj->next->id == id) {
                objtmp = obj->next;
                obj->next = objtmp->next;
                obj = objtmp;
                obj->next = NULL;
                return obj;    
            }
            obj = obj->next;
        }
        obj = NULL;
    }

    return obj;
}

/**
 * Adds a SubDevice to the beginning of the subdev_list list
 */
static void
_subdev_add(InputInfoPtr pInfo, SubDevicePtr subdev) {
    TuioDevicePtr pTuio = pInfo->private;
    SubDevicePtr *subdev_list = &pTuio->subdev_list;
    ObjectPtr obj = pTuio->obj_list;

    if (subdev_list == NULL || subdev == NULL)
        return;

    /* First check to see if there are any objects that don't have a 
     * subdevice that we can assign this subdevice to */
    while (obj != NULL) {
        if (obj->subdev == NULL) {
            obj->subdev = subdev;
            if (pTuio->post_button_events)
                obj->pending.button = True;
            obj->pending.set = True;
            return;
        }
        obj = obj->next;
    }

    /* No subdevice-less objects, add to front of  subdev list */
    if (*subdev_list != NULL) {
        subdev->next = *subdev_list;
    }
    *subdev_list = subdev;
}

/**
 * Gets any available subdevice
 */
static SubDevicePtr
_subdev_get(InputInfoPtr pInfo, SubDevicePtr *subdev_list) {
    SubDevicePtr subdev;

    if (subdev_list == NULL || *subdev_list == NULL) {
        //_hal_create_devices(pInfo, 1);//TODO: HAL BAD NO HAL /* Create one new subdevice */
        return NULL;
    }

    subdev = *subdev_list;
    *subdev_list = subdev->next;
    subdev->next = NULL;

    return subdev;
}

static void
_subdev_remove(InputInfoPtr pInfo, InputInfoPtr sub_pInfo)
{
    TuioDevicePtr pTuio = pInfo->private;
    SubDevicePtr *subdev_list = &pTuio->subdev_list;
    SubDevicePtr subdev = *subdev_list, last;
    ObjectPtr obj = pTuio->obj_list;
    Bool found = False;

    /* First try to find it in the list of subdevices */
    if (subdev != NULL && subdev->pInfo == sub_pInfo) {
        found = True;
        *subdev_list = subdev->next;
        free(subdev);
    } else if (subdev != NULL) {
        last = subdev;
        subdev = subdev->next;
        while (subdev != NULL) {
            if (subdev->pInfo == sub_pInfo) {
                last->next = subdev->next;
                found = True;
                free(subdev);
                break;
            }
            last = subdev;
            subdev = subdev->next;
        }
    }

    /* If it still hasn't been found, find the object that is holding
     * it */
    if (!found) {
        while (obj != NULL) {
            if (obj->subdev != NULL && obj->subdev->pInfo == sub_pInfo) {
                free(obj->subdev);
                obj->subdev = NULL;
                found = True;
                break;
            }
            obj = obj->next;
        }
    }
}

/**
 * Init the button map device.  We only use one button.
 */
static int
_init_buttons(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    CARD8               *map;
    Atom                *labels;
    int numbuttons = 2;
    int                 ret = Success;
    int i;

    map = malloc(numbuttons * sizeof(CARD8));
    labels = malloc(numbuttons * sizeof(Atom));
    for (i=0; i<numbuttons; i++)
        map[i] = i;

    //map = malloc(sizeof(CARD8));
    //*map = 3;
    //label = XIGetKnownProperty("Button Left");

    if (!InitButtonClassDeviceStruct(device, numbuttons/* 1 button */,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                     labels,
#endif
                                     map)) {
        xf86Msg(X_ERROR, "%s: Failed to register buttons.\n", pInfo->name);
        ret = BadAlloc;
    }

    free(labels);
    return ret;
}

/**
 * Init valuators for device, use x/y coordinates.
 */
static int
_init_axes(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    int                 i;
    const int           num_axes = 5;
    Atom *atoms;

    atoms = malloc(num_axes * sizeof(Atom));
    //atom[0] = XI

    if (!InitValuatorClassDeviceStruct(device,
                                       num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                       atoms,
#endif
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 3
                                       GetMotionHistory,
#endif
                                       GetMotionHistorySize(),
                                       0))
        return BadAlloc;

    /* Setup x/y axes */
    for (i = 0; i < 2; i++)
    {
        xf86InitValuatorAxisStruct(device, i,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                   atoms[i],
#endif
                                   0, 0x7FFFFFFF, 1, 1, 1,1);
        xf86InitValuatorDefaults(device, i);
    }

    /* Setup velocity and acceleration axes */
    for (i = 2; i < 6; i++)
    {
        xf86InitValuatorAxisStruct(device, i,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                   atoms[i],
#endif
                                   0x80000000, 0x7FFFFFFF, 1, 1, 1,1);
        xf86InitValuatorDefaults(device, i);
    }

    /* Use absolute mode.  Currently, TUIO coords are mapped to the
     * full screen area */
    //pInfo->dev->valuator->mode = Absolute;
    if (!InitAbsoluteClassDeviceStruct(device))
        return BadAlloc;

    return Success;
}


