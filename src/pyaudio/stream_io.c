#include "stream_io.h"

#include "stream.h"

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include "Python.h"
#include "portaudio.h"

int stream_callback_cfunc(const void *input, void *output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags, void *userData) {
  int return_val = paAbort;
  PyGILState_STATE _state = PyGILState_Ensure();

#ifdef VERBOSE
  if (statusFlags != 0) {
    printf("Status flag set: ");
    if (statusFlags & paInputUnderflow) {
      printf("input underflow!\n");
    }
    if (statusFlags & paInputOverflow) {
      printf("input overflow!\n");
    }
    if (statusFlags & paOutputUnderflow) {
      printf("output underflow!\n");
    }
    if (statusFlags & paOutputUnderflow) {
      printf("output overflow!\n");
    }
    if (statusFlags & paPrimingOutput) {
      printf("priming output!\n");
    }
  }
#endif

  PyAudioCallbackContext *context = (PyAudioCallbackContext *)userData;
  PyObject *py_callback = context->callback;
  unsigned int bytes_per_frame = context->frame_size;
  long main_thread_id = context->main_thread_id;

  PyObject *py_frame_count = PyLong_FromUnsignedLong(frameCount);
  // clang-format off
  PyObject *py_time_info = Py_BuildValue("{s:d,s:d,s:d}",
                                         "input_buffer_adc_time",
                                         timeInfo->inputBufferAdcTime,
                                         "current_time",
                                         timeInfo->currentTime,
                                         "output_buffer_dac_time",
                                         timeInfo->outputBufferDacTime);
  // clang-format on
  PyObject *py_status_flags = PyLong_FromUnsignedLong(statusFlags);
  PyObject *py_input_data = Py_None;
  const char *pData;
  Py_ssize_t output_len;
  PyObject *py_result;

  if (input) {
    py_input_data =
        PyBytes_FromStringAndSize(input, bytes_per_frame * frameCount);
  }

  py_result =
      PyObject_CallFunctionObjArgs(py_callback, py_input_data, py_frame_count,
                                   py_time_info, py_status_flags, NULL);

  if (py_result == NULL) {
#ifdef VERBOSE
    fprintf(stderr, "An error occured while using the portaudio stream\n");
    fprintf(stderr, "Error message: Could not call callback function\n");
#endif
    PyObject *err = PyErr_Occurred();

    if (err) {
      PyThreadState_SetAsyncExc(main_thread_id, err);
      // Print out a stack trace to help debugging.
      // TODO: make VERBOSE a runtime flag so users can control
      // the amount of logging.
      PyErr_Print();
    }

    goto end;
  }

  // clang-format off
  if (!PyArg_ParseTuple(py_result,
                        "z#i",
                        &pData,
                        &output_len,
                        &return_val)) {
// clang-format on
#ifdef VERBOSE
    fprintf(stderr, "An error occured while using the portaudio stream\n");
    fprintf(stderr, "Error message: Could not parse callback return value\n");
#endif

    PyObject *err = PyErr_Occurred();

    if (err) {
      PyThreadState_SetAsyncExc(main_thread_id, err);
      // Print out a stack trace to help debugging.
      // TODO: make VERBOSE a runtime flag so users can control
      // the amount of logging.
      PyErr_Print();
    }

    Py_XDECREF(py_result);
    return_val = paAbort;
    goto end;
  }

  if ((return_val != paComplete) && (return_val != paAbort) &&
      (return_val != paContinue)) {
    PyErr_SetString(PyExc_ValueError,
                    "Invalid PaStreamCallbackResult from callback");
    PyThreadState_SetAsyncExc(main_thread_id, PyErr_Occurred());
    PyErr_Print();

    // Quit the callback loop
    Py_DECREF(py_result);
    return_val = paAbort;
    goto end;
  }

  // Copy bytes for playback only if this is an output stream:
  if (output) {
    char *output_data = (char *)output;
    size_t pa_max_num_bytes = bytes_per_frame * frameCount;
    // Though PyArg_ParseTuple returns the size of pData in output_len, a signed
    // Py_ssize_t, that value should never be negative.
    assert(output_len >= 0);
    // Only copy min(output_len, pa_max_num_bytes) bytes.
    size_t bytes_to_copy = (size_t)output_len < pa_max_num_bytes
                               ? (size_t)output_len
                               : pa_max_num_bytes;
    if (pData != NULL && bytes_to_copy > 0) {
      memcpy(output_data, pData, bytes_to_copy);
    }
    // Pad out the rest of the buffer with 0s if callback returned
    // too few frames (and assume paComplete).
    if (bytes_to_copy < pa_max_num_bytes) {
      memset(output_data + bytes_to_copy, 0, pa_max_num_bytes - bytes_to_copy);
      return_val = paComplete;
    }
  }
  Py_DECREF(py_result);

end:
  if (input) {
    // Decrement this at the end, after memcpy, in case the user
    // returns py_input_data back for playback.
    Py_DECREF(py_input_data);
  }

  Py_XDECREF(py_frame_count);
  Py_XDECREF(py_time_info);
  Py_XDECREF(py_status_flags);

  PyGILState_Release(_state);
  return return_val;
}

/*************************************************************
 * Stream Read/Write
 *************************************************************/

PyObject *pa_write_stream(PyObject *self, PyObject *args) {
  const char *data;
  Py_ssize_t total_size;
  int total_frames;
  int err;
  int should_throw_exception = 0;

  PyObject *stream_arg;
  PyAudioStream *streamObject;

  // clang-format off
  if (!PyArg_ParseTuple(args, "O!s#i|i",
                        &PyAudioStreamType,
                        &stream_arg,
                        &data,
                        &total_size,
                        &total_frames,
                        &should_throw_exception)) {
    return NULL;
  }
  // clang-format on

  if (total_frames < 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid number of frames");
    return NULL;
  }

  streamObject = (PyAudioStream *)stream_arg;

  if (!is_stream_open(streamObject)) {
    PyErr_SetObject(PyExc_IOError,
                    Py_BuildValue("(i,s)", paBadStreamPtr, "Stream closed"));
    return NULL;
  }

  // clang-format off
  Py_BEGIN_ALLOW_THREADS
  err = Pa_WriteStream(streamObject->stream, data, total_frames);
  Py_END_ALLOW_THREADS
  // clang-format on

  if (err != paNoError) {
    if (err == paOutputUnderflowed) {
      if (should_throw_exception) {
        goto error;
      }
    } else
      goto error;
  }

  Py_INCREF(Py_None);
  return Py_None;

error:
  cleanup_stream(streamObject);

#ifdef VERBOSE
  fprintf(stderr, "An error occured while using the portaudio stream\n");
  fprintf(stderr, "Error number: %d\n", err);
  fprintf(stderr, "Error message: %s\n", Pa_GetErrorText(err));
#endif

  PyErr_SetObject(PyExc_IOError,
                  Py_BuildValue("(i,s)", err, Pa_GetErrorText(err)));
  return NULL;
}

PyObject *pa_read_stream(PyObject *self, PyObject *args) {
  int err;
  int total_frames;
  short *sampleBlock;
  int num_bytes;
  PyObject *rv;
  int should_raise_exception = 0;

  PyObject *stream_arg;
  PyAudioStream *streamObject;
  PaStreamParameters *inputParameters;

  // clang-format off
  if (!PyArg_ParseTuple(args, "O!i|i",
                        &PyAudioStreamType,
                        &stream_arg,
                        &total_frames,
                        &should_raise_exception)) {
    return NULL;
  }
  // clang-format on

  if (total_frames < 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid number of frames");
    return NULL;
  }

  streamObject = (PyAudioStream *)stream_arg;

  if (!is_stream_open(streamObject)) {
    PyErr_SetObject(PyExc_IOError,
                    Py_BuildValue("(i,s)", paBadStreamPtr, "Stream closed"));
    return NULL;
  }

  inputParameters = streamObject->inputParameters;
  num_bytes = (total_frames) * (inputParameters->channelCount) *
              (Pa_GetSampleSize(inputParameters->sampleFormat));

#ifdef VERBOSE
  fprintf(stderr, "Allocating %d bytes\n", num_bytes);
#endif

  rv = PyBytes_FromStringAndSize(NULL, num_bytes);
  sampleBlock = (short *)PyBytes_AsString(rv);

  if (sampleBlock == NULL) {
    PyErr_SetObject(PyExc_IOError, Py_BuildValue("(i,s)", paInsufficientMemory,
                                                 "Out of memory"));
    return NULL;
  }

  // clang-format off
  Py_BEGIN_ALLOW_THREADS
  err = Pa_ReadStream(streamObject->stream, sampleBlock, total_frames);
  Py_END_ALLOW_THREADS
  // clang-format on

  if (err != paNoError) {
    if (err == paInputOverflowed) {
      if (should_raise_exception) {
        goto error;
      }
    } else {
      goto error;
    }
  }

  return rv;

error:
  cleanup_stream(streamObject);
  Py_XDECREF(rv);
  PyErr_SetObject(PyExc_IOError,
                  Py_BuildValue("(i,s)", err, Pa_GetErrorText(err)));

#ifdef VERBOSE
  fprintf(stderr, "An error occured while using the portaudio stream\n");
  fprintf(stderr, "Error number: %d\n", err);
  fprintf(stderr, "Error message: %s\n", Pa_GetErrorText(err));
#endif

  return NULL;
}

PyObject *pa_get_stream_write_available(PyObject *self, PyObject *args) {
  signed long frames;
  PyObject *stream_arg;
  PyAudioStream *streamObject;

  if (!PyArg_ParseTuple(args, "O!", &PyAudioStreamType, &stream_arg)) {
    return NULL;
  }

  streamObject = (PyAudioStream *)stream_arg;

  if (!is_stream_open(streamObject)) {
    PyErr_SetObject(PyExc_IOError,
                    Py_BuildValue("(i,s)", paBadStreamPtr, "Stream closed"));
    return NULL;
  }

  // clang-format off
  Py_BEGIN_ALLOW_THREADS
  frames = Pa_GetStreamWriteAvailable(streamObject->stream);
  Py_END_ALLOW_THREADS
  // clang-format on

  return PyLong_FromLong(frames);
}

PyObject *pa_get_stream_read_available(PyObject *self, PyObject *args) {
  signed long frames;
  PyObject *stream_arg;
  PyAudioStream *streamObject;

  if (!PyArg_ParseTuple(args, "O!", &PyAudioStreamType, &stream_arg)) {
    return NULL;
  }

  streamObject = (PyAudioStream *)stream_arg;

  if (!is_stream_open(streamObject)) {
    PyErr_SetObject(PyExc_IOError,
                    Py_BuildValue("(i,s)", paBadStreamPtr, "Stream closed"));
    return NULL;
  }

  // clang-format off
  Py_BEGIN_ALLOW_THREADS
  frames = Pa_GetStreamReadAvailable(streamObject->stream);
  Py_END_ALLOW_THREADS
  // clang-format on

  return PyLong_FromLong(frames);
}
