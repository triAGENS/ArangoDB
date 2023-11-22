#!/usr/bin/env python
""" Run a javascript command by spawning an arangosh
    to the configured connection """

import os
from queue import Queue, Empty
import logging
from datetime import datetime, timedelta
from subprocess import PIPE
from threading import Thread
import psutil
from async_client import (
    ArangoCLIprogressiveTimeoutExecutor,
    print_log,
    default_line_result,
    add_message_to_report,
    kill_children
)

# import tools.loghelper as lh
# pylint: disable=dangerous-default-value


def enqueue_stdout(std_out, queue, instance, identifier, params):
    """add stdout to the specified queue"""
    try:
        for line in iter(std_out.readline, b""):
            # print("O: " + str(line))
            queue.put((line, instance))
    except ValueError as ex:
        print_log(
            f"{identifier} communication line seems to be closed: {str(ex)}", params
        )
    print_log(f"{identifier} x0 done!", params)
    queue.put(-1)
    std_out.close()


def enqueue_stderr(std_err, queue, instance, identifier, params):
    """add stderr to the specified queue"""
    try:
        for line in iter(std_err.readline, b""):
            # print("E: " + str(line))
            queue.put((line, instance))
    except ValueError as ex:
        print_log(
            f"{identifier} communication line seems to be closed: {str(ex)}", params
        )
    print_log(f"{identifier} x1 done!", params)
    queue.put(-1)
    std_err.close()



class ArangoCLIprogressiveTimeoutExecutorWindows(ArangoCLIprogressiveTimeoutExecutor):

    # pylint: disable=too-few-public-methods too-many-arguments disable=too-many-instance-attributes disable=too-many-statements disable=too-many-branches disable=too-many-locals
    def __init__(self, config, connect_instance, deadline_signal=-1):
        super().__init__(config, connect_instance, deadline_signal)

    # fmt: on
    def run_monitored(
        self,
        executable,
        args,
        params={"error": "", "verbose": True, "output": []},
        progressive_timeout=60,
        deadline=0,
        deadline_grace_period=180,
        result_line_handler=default_line_result,
        identifier="",
    ):
        """
        run a script in background tracing with a dynamic timeout that its got output
        Deadline will represent an absolute timeout at which it will be signalled to
        exit, and yet another minute later a hard kill including sub processes will
        follow.
        (is still alive...)
        """
        rc_exit = None
        line_filter = False
        run_cmd = [executable] + args
        children = []
        if identifier == "":
            # pylint: disable=global-statement
            identifier = f"IO_{str(params.my_id)}"
        logging.info(params)
        params["identifier"] = identifier
        if not isinstance(deadline, datetime):
            if deadline == 0:
                deadline = datetime.now() + timedelta(seconds=progressive_timeout * 10)
            else:
                deadline = datetime.now() + timedelta(seconds=deadline)
        final_deadline = deadline + timedelta(seconds=deadline_grace_period)
        logging.info("%s: launching %s", {identifier}, str(run_cmd))
        with psutil.Popen(
            run_cmd,
            stdout=PIPE,
            stderr=PIPE,
            close_fds=ON_POSIX,
            cwd=self.cfg.test_data_dir.resolve(),
            env=self.get_environment(params),
        ) as process:
            # pylint: disable=consider-using-f-string
            params["pid"] = process.pid
            queue = Queue()
            thread1 = Thread(
                name=f"readIO {identifier}",
                target=enqueue_stdout,
                args=(process.stdout, queue, self.connect_instance, identifier, params),
            )
            thread2 = Thread(
                name="readErrIO {identifier}",
                target=enqueue_stderr,
                args=(process.stderr, queue, self.connect_instance, identifier, params),
            )
            thread1.start()
            thread2.start()

            try:
                logging.info(
                    "%s me PID:%s launched PID:%s with LWPID:%s and LWPID:%s",
                    identifier,
                    str(os.getpid()),
                    str(process.pid),
                    str(thread1.native_id),
                    str(thread2.native_id),
                )
            except AttributeError:
                logging.info(
                    "%s me PID:%s launched PID:%s with LWPID:N/A and LWPID:N/A",
                    identifier,
                    str(os.getpid()),
                    str(process.pid),
                )

            # read line without blocking
            have_progressive_timeout = False
            tcount = 0
            close_count = 0
            have_deadline = 0
            deadline_grace_count = 0
            while not have_progressive_timeout:
                # if you want to tail the output, enable this:
                # out.flush()
                result_line_handler(tcount, None, params)
                line = ""
                try:
                    overload = self.cfg.get_overload()
                    if overload:
                        add_message_to_report(params, overload, False)
                    line = queue.get(timeout=1)
                    ret = result_line_handler(0, line, params)
                    line_filter = line_filter or ret
                    tcount = 0
                    if not isinstance(line, tuple):
                        close_count += 1
                        print_log(f"{identifier} 1 IO Thead done!", params)
                        if close_count == 2:
                            break
                except Empty:
                    tcount += 1
                    have_progressive_timeout = tcount >= progressive_timeout
                    if have_progressive_timeout:
                        try:
                            children = process.children(recursive=True)
                        except psutil.NoSuchProcess:
                            pass
                        process.kill()
                        kill_children(identifier, params, children)
                        rc_exit = process.wait()
                    elif tcount % 30 == 0:
                        try:
                            children = children + process.children(recursive=True)
                            rc_exit = process.wait(timeout=1)
                            children = children + self.dig_for_children(params)
                            add_message_to_report(
                                params,
                                f"{identifier} exited unexpectedly: {str(rc_exit)}",
                                True,
                                True,
                            )
                            kill_children(identifier, params, children)
                            break
                        except psutil.NoSuchProcess:
                            children = children + self.dig_for_children(params)
                            add_message_to_report(
                                params,
                                f"{identifier} exited unexpectedly: {str(rc_exit)}",
                                True,
                                True,
                            )
                            kill_children(identifier, params, children)
                            break
                        except psutil.TimeoutExpired:
                            pass  # Wait() has thrown, all is well!
                except OSError as error:
                    logging.error("Got an OS-Error, will abort all! %s", error.strerror)
                    try:
                        # get ALL subprocesses!
                        children = psutil.Process().children(recursive=True)
                    except psutil.NoSuchProcess:
                        pass
                    process.kill()
                    kill_children(identifier, params, children)
                    thread1.join()
                    thread2.join()
                    return {
                        "progressive_timeout": True,
                        "have_deadline": True,
                        "rc_exit": -99,
                        "line_filter": -99,
                    }

                if datetime.now() > deadline:
                    have_deadline += 1
                if have_deadline == 1:
                    # pylint: disable=line-too-long
                    add_message_to_report(
                        params,
                        f"{identifier} Execution Deadline reached - will trigger signal {self.deadline_signal}!",
                    )
                    # Send the process our break / sigint
                    try:
                        children = process.children(recursive=True)
                    except psutil.NoSuchProcess:
                        pass
                    try:
                        process.send_signal(self.deadline_signal)
                    except psutil.NoSuchProcess:
                        children = children + self.dig_for_children(params)
                        print_log(f"{identifier} process already dead!", params)
                elif have_deadline > 1 and datetime.now() > final_deadline:
                    try:
                        # give it some time to exit:
                        print_log(f"{identifier} try wait exit:", params)
                        try:
                            children = children + process.children(recursive=True)
                        except psutil.NoSuchProcess:
                            pass
                        rc_exit = process.wait(1)
                        add_message_to_report(
                            params, f"{identifier}  exited: {str(rc_exit)}"
                        )
                        kill_children(identifier, params, children)
                        print_log(f"{identifier}  closing", params)
                        process.stderr.close()
                        process.stdout.close()
                        break
                    except psutil.TimeoutExpired:
                        # pylint: disable=line-too-long
                        deadline_grace_count += 1
                        print_log(
                            f"{identifier} timeout waiting for exit {str(deadline_grace_count)}",
                            params,
                        )
                        # if its not willing, use force:
                        if deadline_grace_count > deadline_grace_period:
                            print_log(f"{identifier} getting children", params)
                            try:
                                children = process.children(recursive=True)
                            except psutil.NoSuchProcess:
                                pass
                            kill_children(identifier, params, children)
                            add_message_to_report(params, f"{identifier} killing")
                            process.kill()
                            print_log(f"{identifier} waiting", params)
                            rc_exit = process.wait()
                            print_log(f"{identifier} closing", params)
                            process.stderr.close()
                            process.stdout.close()
                            break
            print_log(f"{identifier} IO-Loop done", params)
            timeout_str = ""
            if have_progressive_timeout:
                timeout_str = "TIMEOUT OCCURED!"
                logging.info(timeout_str)
                timeout_str += "\n"
            elif rc_exit is None:
                print_log(f"{identifier} waiting for regular exit", params)
                rc_exit = process.wait()
                print_log(f"{identifier} done", params)
            kill_children(identifier, params, children)
            print_log(f"{identifier} joining io Threads", params)
            thread1.join()
            thread2.join()
            print_log(f"{identifier} OK", params)

        return {
            "progressive_timeout": have_progressive_timeout,
            "have_deadline": have_deadline,
            "rc_exit": rc_exit,
            "line_filter": line_filter,
        }
