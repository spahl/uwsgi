#ifdef UWSGI_EMBEDDED

#include "uwsgi_python.h"

char *spool_buffer = NULL;

extern struct uwsgi_server uwsgi;
extern struct uwsgi_python up;

#define UWSGI_LOGBASE "[- uWSGI -"

char *uwsgi_encode_pydict(PyObject *pydict, uint16_t *size) {

	int i;
	PyObject *zero, *key, *val;
	uint16_t keysize, valsize;


	char *buf, *bufptr;

	PyObject *vars = PyDict_Items(pydict);

        if (!vars) {
		PyErr_Print();
                return NULL;
        }
	
	*size = 0;

	// calc the packet size
	// try to fallback whenever possible
	for (i = 0; i < PyList_Size(vars); i++) {
        	zero = PyList_GetItem(vars, i);
		if (!zero) {
			PyErr_Print();
			continue;
		}

                if (!PyTuple_Check(zero)) {
			uwsgi_log("invalid python dictionary item\n");
			Py_DECREF(zero);
			continue;
		}

		if (PyTuple_Size(zero) < 2) {
			uwsgi_log("invalid python dictionary item\n");
			Py_DECREF(zero);
			continue;
		}
                key = PyTuple_GetItem(zero, 0);
                val = PyTuple_GetItem(zero, 1);

                if (!PyString_Check(key) || !PyString_Check(val)) {
			Py_DECREF(zero);
			continue;
		}


                keysize = PyString_Size(key);
                valsize = PyString_Size(val);

		*size += (keysize + 2 + valsize + 2);

		// do not DECREF here !!!
		//Py_DECREF(zero);
	}

	if (*size <= 4) {
		uwsgi_log("empty python dictionary\n");
		return NULL;
	} 

	// remember to free this memory !!!
	buf = malloc(*size);
	if (!buf) {
		uwsgi_error("malloc()");
		return NULL;
	}

	bufptr = buf;

	for (i = 0; i < PyList_Size(vars); i++) {
                zero = PyList_GetItem(vars, i);
                if (!zero) {
                        PyErr_Print();
                        continue;
                }

                if (!PyTuple_Check(zero)) {
                        uwsgi_log("invalid python dictionary item\n");
                        Py_DECREF(zero);
                        continue;
                }

                if (PyTuple_Size(zero) < 2) {
                        uwsgi_log("invalid python dictionary item\n");
                        Py_DECREF(zero);
                        continue;
                }
                key = PyTuple_GetItem(zero, 0);
                val = PyTuple_GetItem(zero, 1);

                if (!PyString_Check(key) || !PyString_Check(val)) {
                        Py_DECREF(zero);
                        continue;
                }


                keysize = PyString_Size(key);
                valsize = PyString_Size(val);
                if (bufptr + keysize + 2 + valsize + 2 <= buf + *size) {
#ifdef __BIG_ENDIAN__
			keysize = uwsgi_swap16(keysize);
#endif
                        memcpy(bufptr, &keysize, 2);
                        bufptr += 2;
#ifdef __BIG_ENDIAN__
                        keysize = uwsgi_swap16(keysize);
#endif
                        memcpy(bufptr, PyString_AsString(key), keysize);
                        bufptr += keysize;
#ifdef __BIG_ENDIAN__
                        valsize = uwsgi_swap16(valsize);
#endif
                        memcpy(bufptr, &valsize, 2);
                        bufptr += 2;
#ifdef __BIG_ENDIAN__
                        valsize = uwsgi_swap16(valsize);
#endif
                        memcpy(bufptr, PyString_AsString(val), valsize);
                        bufptr += valsize;
		}

		Py_DECREF(zero);

	}

	return buf;

}

PyObject *py_uwsgi_close(PyObject * self, PyObject * args) {

	int fd;

	if (!PyArg_ParseTuple(args, "i:close", &fd)) {
                return NULL;
        }

	close(fd);


	Py_INCREF(Py_None);
	return Py_None;
	
}

PyObject *py_uwsgi_signal(PyObject * self, PyObject * args) {

	char uwsgi_signal;

	if (!PyArg_ParseTuple(args, "B:signal", &uwsgi_signal)) {
                return NULL;
        }

	uwsgi_log("sending %d to master\n", uwsgi_signal);
	write(uwsgi.workers[uwsgi.mywid].pipe[1], &uwsgi_signal, 1);

	Py_INCREF(Py_None);
	return Py_None;
	
}

PyObject *py_uwsgi_recv_frame(PyObject * self, PyObject * args) {

	struct wsgi_request *wsgi_req = current_wsgi_req();

	char *bufptr;
	char prefix = 0x00;
	char suffix = 0xff;
	int i;
	char frame[4096];
	char *frame_ptr;
	int frame_size = 0;
	int fd;
	int rlen;

	int found_start = 0;
	char *null1, *null2;


	if (!PyArg_ParseTuple(args, "icc:recv_frame", &fd, &null1, &null2)) {
                return NULL;
        }

get_data:
	frame_ptr = frame;
	if (wsgi_req->frame_len > 0) {
		// we have already some data buffered
		// search for the prefix and adjust frame_pos
		bufptr = wsgi_req->buffer+wsgi_req->frame_pos;
		for(i=0;i<wsgi_req->frame_len;i++) {
			if (bufptr[i] == prefix) {
				bufptr++;
				found_start = 1;
				break;
			}	
			bufptr++;
			wsgi_req->frame_pos++;
		}

		wsgi_req->frame_len -= i;
		if (found_start) {
			// we have found the prefix, copy it in the frame area until suffix or end of the buffer
			for(i=0;i<wsgi_req->frame_len;i++) {
				uwsgi_log("%d %d\n", bufptr[i], frame_size);
				if (bufptr[i] == suffix) {
					wsgi_req->frame_len -= i;
					goto return_a_frame;
				}
				*frame_ptr++= bufptr[i];
				frame_size++;	
				wsgi_req->frame_pos++;
			}
		}
	}
	
	// we have already get the prefix ?
	if (found_start) {

		// wait for more data
read_more_data:
		rlen = uwsgi_waitfd(fd, -1);
		if (rlen > 0) {
			wsgi_req->frame_pos = 0;
			wsgi_req->frame_len = read(fd, wsgi_req->buffer, uwsgi.buffer_size);
			bufptr = wsgi_req->buffer;
			for(i=0;i<wsgi_req->frame_len;i++) {
				if (bufptr[i] == suffix) {
                                        goto return_a_frame;
                                }
                                *frame_ptr++= bufptr[i];
                                frame_size++;
			}
			goto read_more_data;
		}
		else if (rlen == 0) {
			uwsgi_log("timeout waiting for frame\n");
		}

	}
	else {
		// read a whole frame directly from the socket
		rlen = uwsgi_waitfd(fd, -1);
                if (rlen > 0) {
                        wsgi_req->frame_pos = 0;
                        wsgi_req->frame_len = read(fd, wsgi_req->buffer, uwsgi.buffer_size);
			uwsgi_log("read %d bytes %.*s\n", wsgi_req->frame_len, wsgi_req->frame_len, wsgi_req->buffer);
			if (wsgi_req->frame_len == 0) goto return_a_frame;
                        goto get_data;
                }
                else if (rlen == 0) {
                        uwsgi_log("timeout waiting for frame\n");
                }

	}
return_a_frame:
	uwsgi_log("returning a frame\n");
	return PyString_FromStringAndSize(frame, frame_size);
	
}

PyObject *py_uwsgi_recv_block(PyObject * self, PyObject * args) {
	
	char buf[4096];
	char *bufptr;
	ssize_t rlen = 0, len ;
	int fd, size, remains, ret, timeout = -1;
	

	if (!PyArg_ParseTuple(args, "ii|i:recv_block", &fd, &size, &timeout)) {
                return NULL;
        }

	if (fd < 0) goto clear;

	UWSGI_RELEASE_GIL
	// security check
	if (size > 4096) size = 4096;
	
	remains = size;

	bufptr = buf;
	while(remains > 0) {
		uwsgi_log("%d %d %d\n", remains, size, timeout);
		ret = uwsgi_waitfd(fd, timeout);
		if (ret > 0) {
			len = read(fd,  bufptr, UMIN(remains, size)) ;
			if (len > 0) {
				bufptr+=len;
				rlen += len;
				remains -= len;	
			}
			else {
				break;
			}
		}
		else {
			uwsgi_log("error waiting for block data\n");
			break;
		}
	}

	UWSGI_GET_GIL

	if ( rlen == size) {
		return PyString_FromStringAndSize(buf, rlen);
	}

clear:

	Py_INCREF(Py_None);
	return Py_None;
}

PyObject *py_uwsgi_recv(PyObject * self, PyObject * args) {
	
	int fd, max_size = 4096;
	char buf[4096];
	ssize_t rlen ;
	

	if (!PyArg_ParseTuple(args, "i|i:recv", &fd, &max_size)) {
                return NULL;
        }

	UWSGI_RELEASE_GIL
	// security check
	if (max_size > 4096) max_size = 4096;

	rlen = read(fd,  buf, max_size) ;

	UWSGI_GET_GIL

	if ( rlen > 0) {
		return PyString_FromStringAndSize(buf, rlen);
	}

	Py_INCREF(Py_None);
	return Py_None;
}

PyObject *py_uwsgi_is_connected(PyObject * self, PyObject * args) {

	int fd, soopt;
        socklen_t solen = sizeof(int);

	if (!PyArg_ParseTuple(args, "i:is_connected", &fd)) {
                return NULL;
        }

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *) (&soopt), &solen) < 0) {
                uwsgi_error("getsockopt()");
		goto clear;
        }
        /* is something bad ? */
        if (soopt) goto clear;

	Py_INCREF(Py_True);
	return Py_True;

clear:

	Py_INCREF(Py_None);
	return Py_None;
}


PyObject *py_uwsgi_send(PyObject * self, PyObject * args) {

	PyObject *data;
	PyObject *arg1, *arg2;

	int uwsgi_fd = uwsgi.wsgi_req->poll.fd ;

	if (!PyArg_ParseTuple(args, "O|O:send", &arg1, &arg2)) {
                return NULL;
        }

	if (PyTuple_Size(args) > 1) {
		uwsgi_fd = PyInt_AsLong(arg1);
		data = arg2;
	}
	else {
		data = arg1;
	}

	UWSGI_RELEASE_GIL

	if (write(uwsgi_fd, PyString_AsString(data), PyString_Size(data)) < 0) {
		uwsgi_error("write()");
		UWSGI_GET_GIL
		Py_INCREF(Py_None);
		return Py_None;
	}

	UWSGI_GET_GIL
	Py_INCREF(Py_True);
	return Py_True;

}

#ifdef UWSGI_SENDFILE
PyObject *py_uwsgi_advanced_sendfile(PyObject * self, PyObject * args) {

	PyObject *what;
	char *filename;
	size_t chunk;
	off_t pos = 0;
	size_t filesize = 0;
	struct stat stat_buf;

	int fd = -1;

	if (!PyArg_ParseTuple(args, "O|iii:sendfile", &what, &chunk, &pos, &filesize)) {
		return NULL;
	}

	if (PyString_Check(what)) {

		filename = PyString_AsString(what);

		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			uwsgi_error("open");
			goto clear;
		}

	}
	else {
		fd = PyObject_AsFileDescriptor(what);
		if (fd < 0) goto clear;

		// check for mixing file_wrapper and sendfile
		if (fd == uwsgi.wsgi_req->sendfile_fd) {
			Py_INCREF(what);
		}
	}

	if (!filesize) {
		if (fstat(fd, &stat_buf)) {
			uwsgi_error("fstat()");
			goto clear2;
		}
		else {
			filesize = stat_buf.st_size;
		}

	}

	if (!filesize) goto clear2;

	if (!chunk) chunk = 4096;

	uwsgi.wsgi_req->response_size += uwsgi_do_sendfile(uwsgi.wsgi_req->poll.fd, fd, filesize, chunk, &pos, 0);

	close(fd);
	Py_INCREF(Py_True);
	return Py_True;

clear2:
	close(fd);
clear:
	Py_INCREF(Py_None);
	return Py_None;

}
#endif

#ifdef UWSGI_ASYNC


PyObject *py_uwsgi_async_sleep(PyObject * self, PyObject * args) {

	float timeout;
	time_t sec_timeout;

	if (!PyArg_ParseTuple(args, "f:async_sleep", &timeout)) {
		return NULL;
	}

	sec_timeout = (time_t) timeout;

	if (sec_timeout > 0) {
		async_set_timeout(uwsgi.wsgi_req, sec_timeout);
	}

	return PyString_FromString("");
}
#endif

PyObject *py_uwsgi_warning(PyObject * self, PyObject * args) {
	char *message;
	int len;

	if (!PyArg_ParseTuple(args, "s:set_warning_message", &message)) {
		return NULL;
	}

	len = strlen(message);
	if (len > 80) {
		uwsgi_log( "- warning message must be max 80 chars, it will be truncated -");
		memcpy(uwsgi.shared->warning_message, message, 80);
		uwsgi.shared->warning_message[80] = 0;
	}
	else {
		memcpy(uwsgi.shared->warning_message, message, len);
		uwsgi.shared->warning_message[len] = 0;
	}

	Py_INCREF(Py_True);
	return Py_True;
}

PyObject *py_uwsgi_log(PyObject * self, PyObject * args) {
	char *logline;
	time_t tt;

	if (!PyArg_ParseTuple(args, "s:log", &logline)) {
		return NULL;
	}

	tt = time(NULL);
	if (logline[strlen(logline)] != '\n') {
		uwsgi_log( UWSGI_LOGBASE " %.*s] %s\n", 24, ctime(&tt), logline);
	}
	else {
		uwsgi_log( UWSGI_LOGBASE " %.*s] %s", 24, ctime(&tt), logline);
	}

	Py_INCREF(Py_True);
	return Py_True;
}

PyObject *py_uwsgi_lock(PyObject * self, PyObject * args) {

	// the spooler, the master process or single process environment cannot lock resources
#ifdef UWSGI_SPOOLER
	if (uwsgi.numproc > 1 && uwsgi.mypid != uwsgi.workers[0].pid && uwsgi.mypid != uwsgi.shared->spooler_pid) {
#else
		if (uwsgi.numproc > 1 && uwsgi.mypid != uwsgi.workers[0].pid) {
#endif
			uwsgi_lock(uwsgi.user_lock);
			UWSGI_SET_LOCKING;
		}

		Py_INCREF(Py_None);
		return Py_None;
	}

	PyObject *py_uwsgi_unlock(PyObject * self, PyObject * args) {

		uwsgi_unlock(uwsgi.user_lock);
		UWSGI_UNSET_LOCKING;

		Py_INCREF(Py_None);
		return Py_None;
	}

	PyObject *py_uwsgi_sharedarea_inclong(PyObject * self, PyObject * args) {
		int pos = 0;
		long value = 0;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "ii:sharedarea_inclong", &pos, &value)) {
			return NULL;
		}

		if (pos + 4 >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		memcpy(&value, uwsgi.sharedarea + pos, 4);
		value++;
		memcpy(uwsgi.sharedarea + pos, &value, 4);

		return PyInt_FromLong(value);

	}

	PyObject *py_uwsgi_sharedarea_writelong(PyObject * self, PyObject * args) {
		int pos = 0;
		long value;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "ii:sharedarea_writelong", &pos, &value)) {
			return NULL;
		}

		if (pos + 4 >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		memcpy(uwsgi.sharedarea + pos, &value, 4);

		return PyInt_FromLong(value);

	}

	PyObject *py_uwsgi_sharedarea_write(PyObject * self, PyObject * args) {
		int pos = 0;
		char *value;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "is:sharedarea_write", &pos, &value)) {
			return NULL;
		}

		if (pos + (int) strlen(value) >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		memcpy(uwsgi.sharedarea + pos, value, strlen(value));

		return PyInt_FromLong(strlen(value));

	}

	PyObject *py_uwsgi_sharedarea_writebyte(PyObject * self, PyObject * args) {
		int pos = 0;
		char value;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}


		if (!PyArg_ParseTuple(args, "ib:sharedarea_writebyte", &pos, &value)) {
			return NULL;
		}

		if (pos >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		uwsgi.sharedarea[pos] = value;

		return PyInt_FromLong(uwsgi.sharedarea[pos]);

	}

	PyObject *py_uwsgi_sharedarea_readlong(PyObject * self, PyObject * args) {
		int pos = 0;
		long value;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "i:sharedarea_readlong", &pos)) {
			return NULL;
		}

		if (pos + 4 >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		memcpy(&value, uwsgi.sharedarea + pos, 4);

		return PyInt_FromLong(value);

	}


	PyObject *py_uwsgi_sharedarea_readbyte(PyObject * self, PyObject * args) {
		int pos = 0;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "i:sharedarea_readbyte", &pos)) {
			return NULL;
		}

		if (pos >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		return PyInt_FromLong(uwsgi.sharedarea[pos]);

	}

	PyObject *py_uwsgi_sharedarea_read(PyObject * self, PyObject * args) {
		int pos = 0;
		int len = 1;

		if (uwsgi.sharedareasize <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		if (!PyArg_ParseTuple(args, "i|i:sharedarea_read", &pos, &len)) {
			return NULL;
		}

		if (pos + len >= uwsgi.page_size * uwsgi.sharedareasize) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		return PyString_FromStringAndSize(uwsgi.sharedarea + pos, len);
	}

#ifdef UWSGI_SPOOLER
	PyObject *py_uwsgi_spooler_freq(PyObject * self, PyObject * args) {

		if (!PyArg_ParseTuple(args, "i", &uwsgi.shared->spooler_frequency)) {
			return NULL;
		}

		Py_INCREF(Py_True);
		return Py_True;

	}

	PyObject *py_uwsgi_spooler_jobs(PyObject * self, PyObject * args) {

		DIR *sdir;
		struct dirent *dp;
		char *abs_path;
		struct stat sf_lstat;

		PyObject *jobslist = PyList_New(0);

		sdir = opendir(uwsgi.spool_dir);

		if (sdir) {
			while ((dp = readdir(sdir)) != NULL) {
				if (!strncmp("uwsgi_spoolfile_on_", dp->d_name, 19)) {
					abs_path = malloc(strlen(uwsgi.spool_dir) + 1 + strlen(dp->d_name) + 1);
					if (!abs_path) {
						uwsgi_error("malloc()");
						closedir(sdir);
						goto clear;
					}

					memset(abs_path, 0 , strlen(uwsgi.spool_dir) + 1 + strlen(dp->d_name) + 1);

					memcpy(abs_path, uwsgi.spool_dir, strlen(uwsgi.spool_dir));
					memcpy(abs_path + strlen(uwsgi.spool_dir) , "/", 1);
					memcpy(abs_path + strlen(uwsgi.spool_dir) + 1, dp->d_name, strlen(dp->d_name) );


					if (lstat(abs_path, &sf_lstat)) {
						free(abs_path);
						continue;
					}
					if (!S_ISREG(sf_lstat.st_mode)) {
						free(abs_path);
						continue;
					}
					if (!access(abs_path, R_OK | W_OK)) {
						if (PyList_Append(jobslist, PyString_FromString(abs_path))) {
							PyErr_Print();
						}
					}
					free(abs_path);
				}
			}
			closedir(sdir);
		}

clear:
		return jobslist;

	}


	PyObject *py_uwsgi_send_spool(PyObject * self, PyObject * args) {
		PyObject *spool_dict, *spool_vars;
		PyObject *zero, *key, *val;
		uint16_t keysize, valsize;
		char *cur_buf;
		int i;
		//char spool_filename[1024];

		spool_dict = PyTuple_GetItem(args, 0);
		if (!PyDict_Check(spool_dict)) {
			uwsgi_log("The argument of spooler callable must be a dictionary.\n");
			Py_INCREF(Py_None);
			return Py_None;
		}

		spool_vars = PyDict_Items(spool_dict);
		if (!spool_vars) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		cur_buf = spool_buffer;

		for (i = 0; i < PyList_Size(spool_vars); i++) {
			zero = PyList_GetItem(spool_vars, i);
			if (zero) {
				if (PyTuple_Check(zero)) {
					key = PyTuple_GetItem(zero, 0);
					val = PyTuple_GetItem(zero, 1);

					if (PyString_Check(key) && PyString_Check(val)) {


						keysize = PyString_Size(key);
						valsize = PyString_Size(val);
						if (cur_buf + keysize + 2 + valsize + 2 <= spool_buffer + uwsgi.buffer_size) {

#ifdef __BIG_ENDIAN__
							keysize = uwsgi_swap16(keysize);
#endif
							memcpy(cur_buf, &keysize, 2);
							cur_buf += 2;
#ifdef __BIG_ENDIAN__
							keysize = uwsgi_swap16(keysize);
#endif
							memcpy(cur_buf, PyString_AsString(key), keysize);
							cur_buf += keysize;
#ifdef __BIG_ENDIAN__
							valsize = uwsgi_swap16(valsize);
#endif
							memcpy(cur_buf, &valsize, 2);
							cur_buf += 2;
#ifdef __BIG_ENDIAN__
							valsize = uwsgi_swap16(valsize);
#endif
							memcpy(cur_buf, PyString_AsString(val), valsize);
							cur_buf += valsize;
						}
						else {
							Py_DECREF(zero);
							Py_INCREF(Py_None);
							return Py_None;
						}
					}
					else {
						uwsgi_log("spooler callable dictionary must contains only strings.\n");
						Py_DECREF(zero);
						Py_INCREF(Py_None);
						return Py_None;
					}
				}
				else {
					Py_DECREF(zero);
					Py_INCREF(Py_None);
					return Py_None;
				}
			}
			else {
				Py_INCREF(Py_None);
				return Py_None;
			}
		}

		//i = spool_request(spool_filename, uwsgi.workers[0].requests + 1, spool_buffer, cur_buf - spool_buffer);
		if (i > 0) {
			return Py_True;
		}

		Py_DECREF(spool_vars);
		Py_INCREF(Py_None);
		return Py_None;
	}
#endif

	PyObject *py_uwsgi_send_multi_message(PyObject * self, PyObject * args) {


		int i;
		int clen;
		int pret;
		int managed;
		struct pollfd *multipoll;
		char *buffer;
		struct uwsgi_header uh;
		PyObject *arg_cluster;

		PyObject *cluster_node;

		PyObject *arg_host, *arg_port, *arg_message;

		PyObject *arg_modifier1, *arg_modifier2, *arg_timeout;

		PyObject *marshalled;
		PyObject *retobject;


		arg_cluster = PyTuple_GetItem(args, 0);
		if (!PyTuple_Check(arg_cluster)) {
			Py_INCREF(Py_None);
			return Py_None;
		}


		arg_modifier1 = PyTuple_GetItem(args, 1);
		if (!PyInt_Check(arg_modifier1)) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		arg_modifier2 = PyTuple_GetItem(args, 2);
		if (!PyInt_Check(arg_modifier2)) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		arg_timeout = PyTuple_GetItem(args, 3);
		if (!PyInt_Check(arg_timeout)) {
			Py_INCREF(Py_None);
			return Py_None;
		}


		/* iterate cluster */
		clen = PyTuple_Size(arg_cluster);
		multipoll = malloc(clen * sizeof(struct pollfd));
		if (!multipoll) {
			uwsgi_error("malloc");
			Py_INCREF(Py_None);
			return Py_None;
		}


		buffer = malloc(uwsgi.buffer_size * clen);
		if (!buffer) {
			uwsgi_error("malloc");
			free(multipoll);
			Py_INCREF(Py_None);
			return Py_None;
		}


		for (i = 0; i < clen; i++) {
			multipoll[i].events = POLLIN;

			cluster_node = PyTuple_GetItem(arg_cluster, i);
			arg_host = PyTuple_GetItem(cluster_node, 0);
			if (!PyString_Check(arg_host)) {
				goto clear;
			}

			arg_port = PyTuple_GetItem(cluster_node, 1);
			if (!PyInt_Check(arg_port)) {
				goto clear;
			}

			arg_message = PyTuple_GetItem(cluster_node, 2);
			if (!arg_message) {
				goto clear;
			}


			switch (PyInt_AsLong(arg_modifier1)) {
				case UWSGI_MODIFIER_MESSAGE_MARSHAL:
					marshalled = PyMarshal_WriteObjectToString(arg_message, 1);
					if (!marshalled) {
						PyErr_Print();
						goto clear;
					}
					multipoll[i].fd = uwsgi_enqueue_message(PyString_AsString(arg_host), PyInt_AsLong(arg_port), PyInt_AsLong(arg_modifier1), PyInt_AsLong(arg_modifier2), PyString_AsString(marshalled), PyString_Size(marshalled), PyInt_AsLong(arg_timeout));
					Py_DECREF(marshalled);
					if (multipoll[i].fd < 0) {
						goto multiclear;
					}
					break;
			}


		}

		managed = 0;
		retobject = PyTuple_New(clen);
		if (!retobject) {
			PyErr_Print();
			goto multiclear;
		}

		while (managed < clen) {
			pret = poll(multipoll, clen, PyInt_AsLong(arg_timeout) * 1000);
			if (pret < 0) {
				uwsgi_error("poll()");
				goto megamulticlear;
			}
			else if (pret == 0) {
				uwsgi_log( "timeout on multiple send !\n");
				goto megamulticlear;
			}
			else {
				for (i = 0; i < clen; i++) {
					if (multipoll[i].revents & POLLIN) {
						if (!uwsgi_parse_response(&multipoll[i], PyInt_AsLong(arg_timeout), &uh, &buffer[i])) {
							goto megamulticlear;
						}
						else {
							if (PyTuple_SetItem(retobject, i, PyMarshal_ReadObjectFromString(&buffer[i], uh.pktsize))) {
								PyErr_Print();
								goto megamulticlear;
							}
							close(multipoll[i].fd);
							managed++;
						}
					}
				}
			}
		}

		return retobject;

megamulticlear:

		Py_DECREF(retobject);

multiclear:

		for (i = 0; i < clen; i++) {
			close(multipoll[i].fd);
		}
clear:

		free(multipoll);
		free(buffer);

		Py_INCREF(Py_None);
		return Py_None;

	}


	PyObject *py_uwsgi_get_option(PyObject * self, PyObject * args) {
		int opt_id;

		if (!PyArg_ParseTuple(args, "i:get_option", &opt_id)) {
			return NULL;
		}

		return PyInt_FromLong(uwsgi.shared->options[(uint8_t) opt_id]);
	}

	PyObject *py_uwsgi_set_option(PyObject * self, PyObject * args) {
		int opt_id;
		int value;

		if (!PyArg_ParseTuple(args, "ii:set_option", &opt_id, &value)) {
			return NULL;
		}

		uwsgi.shared->options[(uint8_t) opt_id] = (uint32_t) value;
		return PyInt_FromLong(value);
	}

	PyObject *py_uwsgi_load_plugin(PyObject * self, PyObject * args) {
		int modifier;
		char *plugin_name = NULL;
		char *pargs = NULL;

		if (!PyArg_ParseTuple(args, "is|s:load_plugin", &modifier, &plugin_name, &pargs)) {
			return NULL;
		}

		if (uwsgi_load_plugin(modifier, plugin_name, pargs, 1)) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		Py_INCREF(Py_True);
		return Py_True;
	}

#ifdef UWSGI_MULTICAST
	PyObject *py_uwsgi_multicast(PyObject * self, PyObject * args) {

		char *host, *message;
		ssize_t ret;

		if (!PyArg_ParseTuple(args, "ss:send_multicast_message", &host, &message)) {
			return NULL;
		}

		ret = send_udp_message(UWSGI_MODIFIER_MULTICAST, host, message, strlen(message));

		if (ret <= 0) {
			Py_INCREF(Py_None);
			return Py_None;
		}

		Py_INCREF(Py_True);
		return Py_True;

	}
#endif

	PyObject *py_uwsgi_has_hook(PyObject * self, PyObject * args) {
		int modifier1;

		if (!PyArg_ParseTuple(args, "i:has_hook", &modifier1)) {
			return NULL;
		}

		/*
		   if (uwsgi.shared->hooks[modifier1] != unconfigured_hook) {
		   Py_INCREF(Py_True);
		   return Py_True;
		   }
		   */

		Py_INCREF(Py_None);
		return Py_None;
	}

typedef struct {
	PyObject_HEAD
	int fd;
	int timeout;
	int close;
} uwsgi_Iter;


PyObject* uwsgi_Iter_iter(PyObject *self) {
	Py_INCREF(self);
	return self;
}

PyObject* uwsgi_Iter_next(PyObject *self) {
	int rlen;
	uwsgi_Iter *ui = (uwsgi_Iter *)self;
	char buf[4096];

	UWSGI_RELEASE_GIL
	rlen = uwsgi_waitfd(ui->fd, ui->timeout);
	if (rlen > 0) {
		rlen = read(ui->fd, buf, 4096);
		if (rlen < 0) {
			uwsgi_error("read()");
		}
		else if (rlen > 0) {
			UWSGI_GET_GIL
			return PyString_FromStringAndSize(buf, rlen);
		}
	}
	else if (rlen == 0) {
		uwsgi_log("uwsgi request timed out waiting for response\n");
	}

	if (ui->close) {
		close(ui->fd);
	}
	
	UWSGI_GET_GIL
	PyErr_SetNone(PyExc_StopIteration);

	return NULL;
}

static PyTypeObject uwsgi_IterType = {
    PyObject_HEAD_INIT(NULL)
    0,                          /*ob_size*/
    "uwsgi._Iter",              /*tp_name*/
    sizeof(uwsgi_Iter),         /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    0,                          /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash */
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                         	/*tp_getattro*/
    0,                         	/*tp_setattro*/
    0,                         	/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_ITER,
    "uwsgi response iterator object.",           /* tp_doc */
    0,				/* tp_traverse */
    0,  			/* tp_clear */
    0,  			/* tp_richcompare */
    0,  			/* tp_weaklistoffset */
    uwsgi_Iter_iter,		/* tp_iter: __iter__() method */
    uwsgi_Iter_next		/* tp_iternext: next() method */
};


PyObject *py_uwsgi_async_connect(PyObject * self, PyObject * args) {

	char *socket_name = NULL;
	if (!PyArg_ParseTuple(args, "s:async_connect", &socket_name)) {
                return NULL;
        }

	return PyInt_FromLong(uwsgi_connect(socket_name, 0, 1));
}

PyObject *py_uwsgi_async_send_message(PyObject * self, PyObject * args) {

	PyObject  *pyobj = NULL, *marshalled = NULL;

	int uwsgi_fd;
        int modifier1 = 0;
        int modifier2 = 0;

        ssize_t ret ;

        char *encoded;
        uint16_t esize = 0;

        if (!PyArg_ParseTuple(args, "iiiO:async_send_message", &uwsgi_fd, &modifier1, &modifier2, &pyobj)) {
                return NULL;
        }

        if (uwsgi_fd < 0) goto clear;

        // now check for the type of object to send (fallback to marshal)
        if (PyDict_Check(pyobj)) {
                encoded = uwsgi_encode_pydict(pyobj, &esize);
                if (esize > 0) {
			UWSGI_RELEASE_GIL
                        ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, -1, 0, 0);
                        free(encoded);
                }
        }
        else if (PyString_Check(pyobj)) {
		encoded = PyString_AsString(pyobj);
		esize = PyString_Size(pyobj) ;
		UWSGI_RELEASE_GIL
                ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, -1, 0, 0);
        }
        else {
                marshalled = PyMarshal_WriteObjectToString(pyobj, 1);
                if (!marshalled) {
                        PyErr_Print();
                        goto clear;
                }
	
		encoded = PyString_AsString(marshalled);
		esize = PyString_Size(marshalled);
		UWSGI_RELEASE_GIL
                ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, -1, 0, 0);
        }

	UWSGI_GET_GIL

clear:

        Py_INCREF(Py_None);
        return Py_None;

}

PyObject *py_uwsgi_send_message(PyObject * self, PyObject * args) {

	PyObject *destination = NULL, *pyobj = NULL, *marshalled = NULL;

	int modifier1 = 0;
	int modifier2 = 0;
	int timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];
	int fd = -1;
	int cl = 0;

	ssize_t ret ;

	int uwsgi_fd = -1;
	char *encoded;
	uint16_t esize = 0;
	int close_fd = 0;
	
	uwsgi_Iter *ui;

	if (!PyArg_ParseTuple(args, "OiiO|iii:send_message", &destination, &modifier1, &modifier2, &pyobj, &timeout, &fd, &cl)) {
		return NULL;
	}

	// first of all get the fd for the destination
	if (PyInt_Check(destination)) {
		uwsgi_fd = PyInt_AsLong(destination);
	}
	else if (PyString_Check(destination)) {
		uwsgi_fd = uwsgi_connect(PyString_AsString(destination), timeout, 0);
		close_fd = 1;
	}

	if (uwsgi_fd < 0) goto clear;


	// now check for the type of object to send (fallback to marshal)
	if (PyDict_Check(pyobj)) {
                encoded = uwsgi_encode_pydict(pyobj, &esize);
                if (esize > 0) {
                        UWSGI_RELEASE_GIL
                	ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, fd, cl, timeout);
                        free(encoded);
                }
        }
        else if (PyString_Check(pyobj)) {
                encoded = PyString_AsString(pyobj);
                esize = PyString_Size(pyobj) ;
                UWSGI_RELEASE_GIL
                ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, fd, cl, timeout);
        }
        else {
                marshalled = PyMarshal_WriteObjectToString(pyobj, 1);
                if (!marshalled) {
                        PyErr_Print();
                        goto clear;
                }

                encoded = PyString_AsString(marshalled);
                esize = PyString_Size(marshalled);
                UWSGI_RELEASE_GIL
                ret = uwsgi_send_message(uwsgi_fd, (uint8_t) modifier1, (uint8_t) modifier2, encoded, esize, fd, cl, timeout);
        }

        UWSGI_GET_GIL

	// request sent, return the iterator response
	ui = PyObject_New(uwsgi_Iter, &uwsgi_IterType);
	if (!ui) {
		PyErr_Print();
		goto clear2;
	}

	ui->fd = uwsgi_fd;
	ui->timeout = timeout;
	ui->close = close_fd;
	
	return (PyObject *) ui;

clear2:
	if (close_fd) close(uwsgi_fd);
clear:

	Py_INCREF(Py_None);
	return Py_None;

}

	/* uWSGI masterpid */
	PyObject *py_uwsgi_masterpid(PyObject * self, PyObject * args) {
		if (uwsgi.master_process) {
			return PyInt_FromLong(uwsgi.workers[0].pid);
		}
		return PyInt_FromLong(0);
	}

	/* uWSGI total_requests */
	PyObject *py_uwsgi_total_requests(PyObject * self, PyObject * args) {
		return PyInt_FromLong(uwsgi.workers[0].requests);
	}

	/* uWSGI workers */
	PyObject *py_uwsgi_workers(PyObject * self, PyObject * args) {

		PyObject *worker_dict, *zero;
		int i;

		for (i = 0; i < uwsgi.numproc; i++) {
			worker_dict = PyTuple_GetItem(up.workers_tuple, i);
			if (!worker_dict) {
				goto clear;
			}

			PyDict_Clear(worker_dict);

			zero = PyInt_FromLong(uwsgi.workers[i + 1].id);
			if (PyDict_SetItemString(worker_dict, "id", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyInt_FromLong(uwsgi.workers[i + 1].pid);
			if (PyDict_SetItemString(worker_dict, "pid", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyInt_FromLong(uwsgi.workers[i + 1].requests);
			if (PyDict_SetItemString(worker_dict, "requests", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyInt_FromLong(uwsgi.workers[i + 1].rss_size);
			if (PyDict_SetItemString(worker_dict, "rss", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyInt_FromLong(uwsgi.workers[i + 1].vsz_size);
			if (PyDict_SetItemString(worker_dict, "vsz", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyFloat_FromDouble(uwsgi.workers[i + 1].running_time);
			if (PyDict_SetItemString(worker_dict, "running_time", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyLong_FromLong(uwsgi.workers[i + 1].last_spawn);
			if (PyDict_SetItemString(worker_dict, "last_spawn", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			zero = PyLong_FromLong(uwsgi.workers[i + 1].respawn_count);
			if (PyDict_SetItemString(worker_dict, "respawn_count", zero)) {
				goto clear;
			}
			Py_DECREF(zero);

			/* return a tuple of current status ! (in_request, blocking, locking, )

			   zero = PyLong_FromLong(uwsgi.workers[i+1].in_request);
			   if (PyDict_SetItemString(worker_dict, "in_request", zero)) {
			   goto clear;
			   }
			   Py_DECREF(zero);
			   */

		}


		Py_INCREF(up.workers_tuple);
		return up.workers_tuple;

clear:
		PyErr_Print();
		PyErr_Clear();
		Py_INCREF(Py_None);
		return Py_None;

	}

	/* uWSGI reload */
	PyObject *py_uwsgi_reload(PyObject * self, PyObject * args) {

		if (kill(uwsgi.workers[0].pid, SIGHUP)) {
			uwsgi_error("kill()");
			Py_INCREF(Py_None);
			return Py_None;
		}

		Py_INCREF(Py_True);
		return Py_True;
	}

	/* blocking hint */
	PyObject *py_uwsgi_set_blocking(PyObject * self, PyObject * args) {

		if (uwsgi.master_process) {
			uwsgi.workers[uwsgi.mywid].status |= UWSGI_STATUS_BLOCKING;
			Py_INCREF(Py_True);
			return Py_True;
		}


		Py_INCREF(Py_None);
		return Py_None;
	}


	PyObject *py_uwsgi_request_id(PyObject * self, PyObject * args) {
		return PyInt_FromLong(uwsgi.workers[uwsgi.mywid].requests);
	}

	PyObject *py_uwsgi_worker_id(PyObject * self, PyObject * args) {
		return PyInt_FromLong(uwsgi.mywid);
	}

	PyObject *py_uwsgi_logsize(PyObject * self, PyObject * args) {
		return PyInt_FromLong(uwsgi.shared->logsize);
	}

	PyObject *py_uwsgi_mem(PyObject * self, PyObject * args) {

		PyObject *ml = PyTuple_New(2);

		get_memusage();

		PyTuple_SetItem(ml, 0, PyLong_FromLong(uwsgi.workers[uwsgi.mywid].rss_size));
		PyTuple_SetItem(ml, 1, PyLong_FromLong(uwsgi.workers[uwsgi.mywid].vsz_size));

		return ml;

	}

PyObject *py_uwsgi_cl(PyObject * self, PyObject * args) {

	struct wsgi_request *wsgi_req = current_wsgi_req();

	return PyLong_FromLong(wsgi_req->post_cl);

}

	PyObject *py_uwsgi_disconnect(PyObject * self, PyObject * args) {

		struct wsgi_request *wsgi_req = current_wsgi_req();

		uwsgi_log( "disconnecting worker %d (pid :%d) from session...\n", uwsgi.mywid, uwsgi.mypid);

		fclose(wsgi_req->async_post);
		wsgi_req->fd_closed = 1;

		Py_INCREF(Py_True);
		return Py_True;
	}

	PyObject *py_uwsgi_parse_file(PyObject * self, PyObject * args) {

		char *filename;
		int fd;
		ssize_t len;
		char *buffer, *ptrbuf, *bufferend, *keybuf;
		uint16_t strsize = 0, keysize = 0;

		struct uwsgi_header uh;
		PyObject *zero;

		if (!PyArg_ParseTuple(args, "s:parsefile", &filename)) {
			return NULL;
		}

		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			uwsgi_error("open()");
			goto clear;
		}

		len = read(fd, &uh, 4);
		if (len != 4) {
			uwsgi_error("read()");
			goto clear2;
		}

		buffer = malloc(uh.pktsize);
		if (!buffer) {
			uwsgi_error("malloc()");
			goto clear2;
		}
		len = read(fd, buffer, uh.pktsize);
		if (len != uh.pktsize) {
			uwsgi_error("read()");
			free(buffer);
			goto clear2;
		}

		ptrbuf = buffer;
		bufferend = ptrbuf + uh.pktsize;

		if (!uh.modifier1 || uh.modifier1 == UWSGI_MODIFIER_SPOOL_REQUEST) {
			zero = PyDict_New();

			while (ptrbuf < bufferend) {
				if (ptrbuf + 2 < bufferend) {
					memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
					strsize = uwsgi_swap16(strsize);
#endif
					/* key cannot be null */
					if (!strsize) {
						uwsgi_log( "uwsgi key cannot be null.\n");
						goto clear3;
					}

					ptrbuf += 2;
					if (ptrbuf + strsize < bufferend) {
						// var key
						keybuf = ptrbuf;
						keysize = strsize;
						ptrbuf += strsize;
						// value can be null (even at the end) so use <=
						if (ptrbuf + 2 <= bufferend) {
							memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
							strsize = uwsgi_swap16(strsize);
#endif
							ptrbuf += 2;
							if (ptrbuf + strsize <= bufferend) {
								PyDict_SetItem(zero, PyString_FromStringAndSize( keybuf, keysize ), PyString_FromStringAndSize( ptrbuf, strsize ));
								ptrbuf += strsize;
							}
							else {
								goto clear3;
							}
						}
						else {
							goto clear3;
						}
					}
				}
				else {
					goto clear3;
				}
			}

			return zero;

		}

		goto clear;

clear3:
		Py_DECREF(zero);
clear2:
		close(fd);
clear:
		Py_INCREF(Py_None);
		return Py_None;

	}

	PyObject *py_uwsgi_grunt(PyObject * self, PyObject * args) {

		pid_t grunt_pid;
		int i;
		struct wsgi_request *wsgi_req = current_wsgi_req();

		if (uwsgi.grunt) {
			uwsgi_log( "spawning a grunt from worker %d (pid :%d)...\n", uwsgi.mywid, uwsgi.mypid);
		}
		else {
			uwsgi_log( "grunt support is disabled !!!\n" );
			goto clear;
		}

		grunt_pid = fork();
		if (grunt_pid < 0) {
			uwsgi_error("fork()");
			goto clear;
		}
		else if (grunt_pid == 0) {
			for(i=0;i<uwsgi.sockets_cnt;i++) {
				close(uwsgi.sockets[i].fd);
			}
			// create a new session
			setsid();
			// exit on SIGPIPE
			signal(SIGPIPE, (void *) &end_me);
			uwsgi.mywid = uwsgi.numproc + 1;
			uwsgi.mypid = getpid();
			memset(&uwsgi.workers[uwsgi.mywid], 0, sizeof(struct uwsgi_worker));
			// this is pratically useless...
			uwsgi.workers[uwsgi.mywid].id = uwsgi.mywid;
			// this field will be overwrite after each call
			uwsgi.workers[uwsgi.mywid].pid = uwsgi.mypid;
			// TODO
			// manage thread in grunt processes
			Py_INCREF(Py_True);
			return Py_True;
		}

		// close connection on the worker
		fclose(wsgi_req->async_post);
		wsgi_req->fd_closed = 1;

clear:
		Py_INCREF(Py_None);
		return Py_None;
	}

#ifdef UWSGI_SPOOLER
	static PyMethodDef uwsgi_spooler_methods[] = {
		{"send_to_spooler", py_uwsgi_send_spool, METH_VARARGS, ""},
		{"set_spooler_frequency", py_uwsgi_spooler_freq, METH_VARARGS, ""},
		{"spooler_jobs", py_uwsgi_spooler_jobs, METH_VARARGS, ""},
		{NULL, NULL},
	};
#endif


PyObject *py_uwsgi_suspend(PyObject * self, PyObject * args) {

        struct wsgi_request *wsgi_req = current_wsgi_req();

        uwsgi.schedule_to_main(wsgi_req);

        Py_INCREF(Py_True);
        return Py_True;

}

#ifdef UWSGI_MULTICAST
PyObject *py_uwsgi_cluster_nodes(PyObject * self, PyObject * args) {

	struct uwsgi_cluster_node *ucn;
	int i;

	PyObject *clist = PyList_New(0);

	for (i = 0; i < MAX_CLUSTER_NODES; i++) {
                ucn = &uwsgi.shared->nodes[i];
                if (ucn->name[0] != 0) {
			if (ucn->status == UWSGI_NODE_OK) {
				PyList_Append(clist, PyString_FromString(ucn->name));
			}
                }
        }

	return clist;

}

PyObject *py_uwsgi_cluster_best_node(PyObject * self, PyObject * args) {

	char *node = uwsgi_cluster_best_node() ;
	if (node == NULL) goto clear;
	if (node[0] == 0) goto clear;
	return PyString_FromString(node);

clear:

	Py_INCREF(Py_None);
	return Py_None;
}


#endif


static PyMethodDef uwsgi_advanced_methods[] = {
		{"send_message", py_uwsgi_send_message, METH_VARARGS, ""},
		{"send_multi_message", py_uwsgi_send_multi_message, METH_VARARGS, ""},
		{"reload", py_uwsgi_reload, METH_VARARGS, ""},
		{"workers", py_uwsgi_workers, METH_VARARGS, ""},
		{"masterpid", py_uwsgi_masterpid, METH_VARARGS, ""},
		{"total_requests", py_uwsgi_total_requests, METH_VARARGS, ""},
		{"getoption", py_uwsgi_get_option, METH_VARARGS, ""},
		{"get_option", py_uwsgi_get_option, METH_VARARGS, ""},
		{"setoption", py_uwsgi_set_option, METH_VARARGS, ""},
		{"set_option", py_uwsgi_set_option, METH_VARARGS, ""},
		{"sorry_i_need_to_block", py_uwsgi_set_blocking, METH_VARARGS, ""},
		{"request_id", py_uwsgi_request_id, METH_VARARGS, ""},
		{"worker_id", py_uwsgi_worker_id, METH_VARARGS, ""},
		{"log", py_uwsgi_log, METH_VARARGS, ""},
		{"disconnect", py_uwsgi_disconnect, METH_VARARGS, ""},
		{"grunt", py_uwsgi_grunt, METH_VARARGS, ""},
		{"load_plugin", py_uwsgi_load_plugin, METH_VARARGS, ""},
		{"lock", py_uwsgi_lock, METH_VARARGS, ""},
		{"unlock", py_uwsgi_unlock, METH_VARARGS, ""},
		{"send", py_uwsgi_send, METH_VARARGS, ""},
		{"cl", py_uwsgi_cl, METH_VARARGS, ""},

		{"signal", py_uwsgi_signal, METH_VARARGS, ""},
#ifdef UWSGI_SENDFILE
		{"sendfile", py_uwsgi_advanced_sendfile, METH_VARARGS, ""},
#endif
		{"set_warning_message", py_uwsgi_warning, METH_VARARGS, ""},
		{"mem", py_uwsgi_mem, METH_VARARGS, ""},
		{"has_hook", py_uwsgi_has_hook, METH_VARARGS, ""},
		{"logsize", py_uwsgi_logsize, METH_VARARGS, ""},
#ifdef UWSGI_MULTICAST
		{"send_multicast_message", py_uwsgi_multicast, METH_VARARGS, ""},
		{"cluster_nodes", py_uwsgi_cluster_nodes, METH_VARARGS, ""},
		{"cluster_best_node", py_uwsgi_cluster_best_node, METH_VARARGS, ""},
#endif
#ifdef UWSGI_ASYNC
		{"async_sleep", py_uwsgi_async_sleep, METH_VARARGS, ""},
		{"async_connect", py_uwsgi_async_connect, METH_VARARGS, ""},
		{"async_send_message", py_uwsgi_async_send_message, METH_VARARGS, ""},

		{"green_schedule", py_uwsgi_suspend, METH_VARARGS, ""},
		{"suspend", py_uwsgi_suspend, METH_VARARGS, ""},
		{"wait_fd_read", py_eventfd_read, METH_VARARGS, ""},
		{"wait_fd_write", py_eventfd_write, METH_VARARGS, ""},
#endif
		{"is_connected", py_uwsgi_is_connected, METH_VARARGS, ""},
		{"send", py_uwsgi_send, METH_VARARGS, ""},
		{"recv", py_uwsgi_recv, METH_VARARGS, ""},
		{"recv_block", py_uwsgi_recv_block, METH_VARARGS, ""},
		{"recv_frame", py_uwsgi_recv_frame, METH_VARARGS, ""},
		{"close", py_uwsgi_close, METH_VARARGS, ""},
		
		{"parsefile", py_uwsgi_parse_file, METH_VARARGS, ""},
		//{"call_hook", py_uwsgi_call_hook, METH_VARARGS, ""},
		
		{NULL, NULL},
};


static PyMethodDef uwsgi_sa_methods[] = {
	{"sharedarea_read", py_uwsgi_sharedarea_read, METH_VARARGS, ""},
	{"sharedarea_write", py_uwsgi_sharedarea_write, METH_VARARGS, ""},
	{"sharedarea_readbyte", py_uwsgi_sharedarea_readbyte, METH_VARARGS, ""},
	{"sharedarea_writebyte", py_uwsgi_sharedarea_writebyte, METH_VARARGS, ""},
	{"sharedarea_readlong", py_uwsgi_sharedarea_readlong, METH_VARARGS, ""},
	{"sharedarea_writelong", py_uwsgi_sharedarea_writelong, METH_VARARGS, ""},
	{"sharedarea_inclong", py_uwsgi_sharedarea_inclong, METH_VARARGS, ""},
	{NULL, NULL},
};

PyObject *py_uwsgi_cache_del(PyObject * self, PyObject * args) {

        char *key ;
        char *value ;

        if (!PyArg_ParseTuple(args, "s:cache_del", &key, &value)) {
                return NULL;
        }


        if (uwsgi_cache_del(key, strlen(key))) {
                Py_INCREF(Py_None);
                return Py_None;
        }

        Py_INCREF(Py_True);
        return Py_True;

}


PyObject *py_uwsgi_cache_set(PyObject * self, PyObject * args) {

        char *key ;
        char *value ;
	uint64_t expires = 0;

        if (!PyArg_ParseTuple(args, "ss|i:cache_set", &key, &value, &expires)) {
                return NULL;
        }


        if (uwsgi_cache_set(key, strlen(key), value, strlen(value), expires)) {
        	Py_INCREF(Py_None);
        	return Py_None;
        }

	Py_INCREF(Py_True);
        return Py_True;

}


PyObject *py_uwsgi_cache_get(PyObject * self, PyObject * args) {

	char *key ;
	uint16_t valsize;
	char *value ;
	
	if (!PyArg_ParseTuple(args, "s:cache_get", &key)) {
                return NULL;
        }

	value = uwsgi_cache_get(key, strlen(key), &valsize);
	
	if (value) {
		return PyString_FromStringAndSize(value, valsize);
	}
	
	Py_INCREF(Py_None);
	return Py_None;

}

static PyMethodDef uwsgi_cache_methods[] = {
	{"cache_get", py_uwsgi_cache_get, METH_VARARGS, ""},
	{"cache_set", py_uwsgi_cache_set, METH_VARARGS, ""},
	{"cache_del", py_uwsgi_cache_del, METH_VARARGS, ""},
	{NULL, NULL},
};



#ifdef UWSGI_SPOOLER
	void init_uwsgi_module_spooler(PyObject * current_uwsgi_module) {
		PyMethodDef *uwsgi_function;
		PyObject *uwsgi_module_dict;

		uwsgi_module_dict = PyModule_GetDict(current_uwsgi_module);
		if (!uwsgi_module_dict) {
			uwsgi_log( "could not get uwsgi module __dict__\n");
			exit(1);
		}

		spool_buffer = malloc(uwsgi.buffer_size);
		if (!spool_buffer) {
			uwsgi_error("malloc()");
			exit(1);
		}


		for (uwsgi_function = uwsgi_spooler_methods; uwsgi_function->ml_name != NULL; uwsgi_function++) {
			PyObject *func = PyCFunction_New(uwsgi_function, NULL);
			PyDict_SetItemString(uwsgi_module_dict, uwsgi_function->ml_name, func);
			Py_DECREF(func);
		}
	}
#endif

void init_uwsgi_module_advanced(PyObject * current_uwsgi_module) {
		PyMethodDef *uwsgi_function;
		PyObject *uwsgi_module_dict;

		uwsgi_module_dict = PyModule_GetDict(current_uwsgi_module);
		if (!uwsgi_module_dict) {
			uwsgi_log( "could not get uwsgi module __dict__\n");
			exit(1);
		}

	uwsgi_IterType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&uwsgi_IterType) < 0) {
		PyErr_Print();
		exit(1);
	}

	for (uwsgi_function = uwsgi_advanced_methods; uwsgi_function->ml_name != NULL; uwsgi_function++) {
		PyObject *func = PyCFunction_New(uwsgi_function, NULL);
		PyDict_SetItemString(uwsgi_module_dict, uwsgi_function->ml_name, func);
		Py_DECREF(func);
	}

}

	void init_uwsgi_module_cache(PyObject * current_uwsgi_module) {
		PyMethodDef *uwsgi_function;
		PyObject *uwsgi_module_dict;

		uwsgi_module_dict = PyModule_GetDict(current_uwsgi_module);
		if (!uwsgi_module_dict) {
			uwsgi_log( "could not get uwsgi module __dict__\n");
			exit(1);
		}

		for (uwsgi_function = uwsgi_cache_methods; uwsgi_function->ml_name != NULL; uwsgi_function++) {
			PyObject *func = PyCFunction_New(uwsgi_function, NULL);
			PyDict_SetItemString(uwsgi_module_dict, uwsgi_function->ml_name, func);
			Py_DECREF(func);
		}
	}

	void init_uwsgi_module_sharedarea(PyObject * current_uwsgi_module) {
		PyMethodDef *uwsgi_function;
		PyObject *uwsgi_module_dict;

		uwsgi_module_dict = PyModule_GetDict(current_uwsgi_module);
		if (!uwsgi_module_dict) {
			uwsgi_log( "could not get uwsgi module __dict__\n");
			exit(1);
		}

		for (uwsgi_function = uwsgi_sa_methods; uwsgi_function->ml_name != NULL; uwsgi_function++) {
			PyObject *func = PyCFunction_New(uwsgi_function, NULL);
			PyDict_SetItemString(uwsgi_module_dict, uwsgi_function->ml_name, func);
			Py_DECREF(func);
		}
	}

#endif