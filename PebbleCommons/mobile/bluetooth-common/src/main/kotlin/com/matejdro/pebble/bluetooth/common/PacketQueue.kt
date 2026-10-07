package com.matejdro.pebble.bluetooth.common

import com.matejdro.pebble.bluetooth.common.di.WatchappConnectionScope
import com.matejdro.pebble.bluetooth.common.exceptions.UnrecoverableWatchTransferException
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.TransmissionResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.delay
import logcat.logcat
import java.util.PriorityQueue
import java.util.UUID
import kotlin.coroutines.cancellation.CancellationException
import kotlin.time.Duration.Companion.milliseconds

@Inject
@SingleIn(WatchappConnectionScope::class)
class PacketQueue(
   private val sender: PebbleSender,
   private val watch: WatchIdentifier,
   @WatchappId
   private val watchappUuid: UUID,
) {
   private val newPacketNotification = Channel<Unit>(Channel.CONFLATED)

   private val queue = PriorityQueue<Packet>()
   private var nextSequenceNumber = 0L
   @Volatile
   private var closed: CancellationException? = null

   /**
    * (Eventually) send a packet to the watch.
    *
    * This will insert the packet into the queue and suspend until the packet is sent.
    *
    * If this coroutine is cancelled before the packet is sent, the packet will be removed from the queue.
    */
   suspend fun sendPacket(dictionary: PebbleDictionary, priority: Int = 0) =
      sendPackets(listOf(dictionary), priority)

   /** Send a complete transfer without interleaving another transfer's chunks. */
   @Suppress("SuspendFunSwallowedCancellation")
   suspend fun sendPackets(dictionaries: List<PebbleDictionary>, priority: Int = 0) {
      if (dictionaries.isEmpty()) return
      val completion = CompletableDeferred<Unit>()
      val packet = Packet(dictionaries, priority, nextPacketSequenceNumber(), completion)
      addPacket(packet)
      try {
         completion.await()
      } catch (e: CancellationException) {
         packet.cancelled.complete(Unit)
         synchronized(queue) { queue.remove(packet) }
         throw e
      }
   }

   fun enqueuePacket(dictionary: PebbleDictionary, priority: Int = 0) =
      enqueuePackets(listOf(dictionary), priority)

   fun enqueuePackets(dictionaries: List<PebbleDictionary>, priority: Int = 0) {
      if (dictionaries.isEmpty()) return
      addPacket(Packet(dictionaries, priority, nextPacketSequenceNumber(), null))
   }

   private fun addPacket(packet: Packet) {
      synchronized(queue) {
         closed?.let { throw it }
         queue.add(packet)
      }
      newPacketNotification.trySend(Unit)
   }

   /**
    * Process packets in this queue. This will suspend indefinitely.
    */
   @Suppress("SuspendFunSwallowedCancellation") // We clear the packets before re-throwing the exception
   suspend fun runQueue(): Nothing {
      try {
         while (true) {
            val nextPacket = synchronized(queue) {
               queue.poll()
            }

            if (nextPacket == null) {
               newPacketNotification.receive()
               continue
            }

            coroutineScope {
               val sending = async {
                  try {
                     for (dictionary in nextPacket.dictionaries) {
                        transmit(dictionary)
                     }
                     nextPacket.sentNofification?.complete(Unit)
                  } catch (e: CancellationException) {
                     nextPacket.sentNofification?.cancel(e)
                     throw e
                  } catch (e: Exception) {
                     nextPacket.sentNofification?.completeExceptionally(e)
                     logcat { "Transfer failed: $e" }
                  }
               }
               select<Unit> {
                  sending.onAwait { }
                  nextPacket.cancelled.onAwait { sending.cancel() }
               }
            }
         }
      } catch (e: CancellationException) {
         val finalPackets = synchronized(queue) {
            closed = e
            queue.toList().also { queue.clear() }
         }

         for (packet in finalPackets) {
            packet.sentNofification?.cancel(e)
         }

         throw e
      }
   }

   private suspend fun transmit(dictionary: PebbleDictionary) {
      var nextRetryDelay = START_RETRY_DELAY
      repeat(MAX_SEND_ATTEMPTS) { attempt ->
         val result = withTimeoutOrNull(SEND_TIMEOUT) {
            sender.sendDataToPebble(watchappUuid, dictionary, listOf(watch))
               ?: throw UnrecoverableWatchTransferException("No Pebble app is installed")
         }
         val watchResult = result?.get(watch) ?: if (result == null) TransmissionResult.FailedTimeout else null
         when (watchResult) {
            TransmissionResult.Success -> return
            TransmissionResult.FailedTimeout,
            TransmissionResult.FailedWatchNotConnected,
            TransmissionResult.FailedWatchNacked,
            -> {
               if (attempt == MAX_SEND_ATTEMPTS - 1) {
                  throw UnrecoverableWatchTransferException("Retry limit reached: $watchResult")
               }
               delay(nextRetryDelay)
               nextRetryDelay = (nextRetryDelay * 2).coerceAtMost(MAX_RETRY_DELAY)
            }
            else -> throw UnrecoverableWatchTransferException(watchResult?.toString())
         }
      }
   }

   private class Packet(
      val dictionaries: List<PebbleDictionary>,
      val priority: Int,
      val sequenceNumber: Long,
      val sentNofification: CompletableDeferred<Unit>?,
   ) : Comparable<Packet> {
      val cancelled = CompletableDeferred<Unit>()
      override fun compareTo(other: Packet): Int {
         val priorityComparison = -priority.compareTo(other.priority)
         if (priorityComparison != 0) {
            return priorityComparison
         }
         return sequenceNumber.compareTo(other.sequenceNumber)
      }
   }

   private fun nextPacketSequenceNumber(): Long {
      return synchronized(queue) {
         nextSequenceNumber++
      }
   }
}

private val START_RETRY_DELAY = 100.milliseconds

private val MAX_RETRY_DELAY = 2_000.milliseconds
private val SEND_TIMEOUT = 5_000.milliseconds
private const val MAX_SEND_ATTEMPTS = 8
