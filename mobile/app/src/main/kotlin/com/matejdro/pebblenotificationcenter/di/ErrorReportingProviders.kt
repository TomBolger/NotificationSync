package com.matejdro.pebblenotificationcenter.di

import android.content.Context
import com.matejdro.pebble.common.crashreport.CrashReportService
import com.matejdro.pebble.common.logging.TinyLogLoggingThread
import com.matejdro.pebblenotificationcenter.BuildConfig
import com.matejdro.pebblenotificationcenter.common.exceptions.CrashOnDebugException
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesTo
import dev.zacsweers.metro.Provides
import org.tinylog.Level
import si.inova.kotlinova.core.exceptions.UnknownCauseException
import si.inova.kotlinova.core.outcome.CauseException
import si.inova.kotlinova.core.reporting.ErrorReporter

@Suppress("unused")
@ContributesTo(AppScope::class)
interface ErrorReportingProviders {
   @Provides
   fun provideErrorReporter(tinyLogLoggingThread: TinyLogLoggingThread, context: Context): ErrorReporter {
      return object : ErrorReporter {
         override fun report(throwable: Throwable) {
            if (isWatchLinkProblem(throwable)) {
               // The watch went away mid-send (you left the watch app, walked out of range...). That's normal and
               // the app recovers on its own, so it goes to the log instead of a crash notification.
               tinyLogLoggingThread.log(1, "ErrorReporter", Level.WARN, null, throwable)
               return
            }

            if (throwable !is CauseException) {
               report(UnknownCauseException("Got reported non-cause exception", throwable))
               return
            }

            if (throwable.shouldReport) {
               throwable.printStackTrace()
               tinyLogLoggingThread.log(1, "ErrorReporter", Level.ERROR, null, throwable)
               CrashReportService.showCrashNotification(throwable.stackTraceToString(), context)
            } else if (BuildConfig.DEBUG) {
               if (throwable is CrashOnDebugException) {
                  throw throwable
               }
               throwable.printStackTrace()
            }
         }
      }
   }
}

private fun isWatchLinkProblem(throwable: Throwable): Boolean {
   return generateSequence(throwable) { it.cause }
      .take(MAX_CAUSE_DEPTH)
      .any { it.javaClass.simpleName == "UnrecoverableWatchTransferException" }
}

private const val MAX_CAUSE_DEPTH = 10
