package com.matejdro.pebble.bluetooth.common

import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dispatch.core.DefaultCoroutineScope
import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.ReceiveResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.job
import logcat.logcat
import si.inova.kotlinova.core.exceptions.UnknownCauseException
import si.inova.kotlinova.core.reporting.ErrorReporter
import java.util.UUID

@Inject
@ContributesBinding(AppScope::class)
class WatchappConnectionsManagerImpl(
   private val defaultScope: DefaultCoroutineScope,
   private val connectionFactory: WatchAppConnection.Factory,
   private val errorReporter: ErrorReporter,
   @WatchappId
   private val targetWatchappUUID: UUID,
) : WatchappConnectionsManager {
   private val activeConnections: MutableMap<WatchIdentifier, ConnectionWrapper> = mutableMapOf()
   override suspend fun onMessageReceived(
      watchappUUID: UUID,
      data: PebbleDictionary,
      watch: WatchIdentifier,
   ): ReceiveResult {
      logcat { "onMessageReceived: $watchappUUID $watch" }

      if (watchappUUID != this.targetWatchappUUID) {
         errorReporter.report(UnknownCauseException("Got app opened for the unknown app"))
         return ReceiveResult.Nack
      }

      val connection = synchronized(activeConnections) {
         activeConnections[watch] ?: createConnection(watch, createdByMessage = true)
      }

      return try {
         connection.connection.onPacketReceived(data)
      } catch (e: CancellationException) {
         throw e
      } catch (e: Throwable) {
         errorReporter.report(e)
         ReceiveResult.Nack
      }
   }

   override fun onAppOpened(watchappUUID: UUID, watch: WatchIdentifier) {
      logcat { "onAppOpened: $watchappUUID $watch" }
      if (watchappUUID != this.targetWatchappUUID) {
         errorReporter.report(UnknownCauseException("Got app opened for the unknown app"))
         return
      }

      synchronized(activeConnections) {
         val existing = activeConnections[watch]
         if (existing != null) {
            if (existing.createdByMessage && System.nanoTime() - existing.createdAtNanos < FRESH_CONNECTION_NANOS) {
               // The watch's first message can overtake the "app opened" event. That connection was made for this
               // very launch and may already be answering the hello; replacing it would cost the watch a retry.
               logcat { "Connection for the $watch was just created for this launch, keeping it" }
               return
            }
            logcat { "Connection for the $watch is already opened, replacing stale connection" }
            activeConnections.remove(watch)?.scope?.cancel()
         }

         createConnection(watch)
      }
   }

   private fun createConnection(watch: WatchIdentifier, createdByMessage: Boolean = false): ConnectionWrapper {
      val newScope = CoroutineScope(
         defaultScope.coroutineContext +
            CoroutineExceptionHandler { _, throwable -> errorReporter.report(throwable) } +
            SupervisorJob(defaultScope.coroutineContext.job)
      )

      val newConnection = connectionFactory.create(watch, newScope)
      val wrapper = ConnectionWrapper(newConnection, newScope, System.nanoTime(), createdByMessage)
      activeConnections[watch] = wrapper
      return wrapper
   }

   override fun onAppClosed(watchappUUID: UUID, watch: WatchIdentifier) {
      logcat { "onAppClosed: $watchappUUID $watch" }

      synchronized(activeConnections) {
         activeConnections.remove(watch)?.scope?.cancel()
      }
   }

   private data class ConnectionWrapper(
      val connection: WatchAppConnection,
      val scope: CoroutineScope,
      val createdAtNanos: Long,
      val createdByMessage: Boolean,
   )
}

private const val FRESH_CONNECTION_NANOS = 3_000_000_000L
