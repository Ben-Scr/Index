using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace Index.Collections.Native
{
    public unsafe sealed class NativeArray<T> : IDisposable where T : unmanaged
    {
        private T* buffer;
        private int length;
        private int version;
        private int disposeState;

        public int Length
        {
            get
            {
                ThrowIfDisposed();
                return length;
            }
        }

        public bool IsCreated => Volatile.Read(ref disposeState) == 0 && buffer != null;

        ~NativeArray()
        {
            Dispose(false);
        }

        public NativeArray(int length)
        {
            if (length < 0)
                throw new ArgumentOutOfRangeException(nameof(length));

            if (length == 0)
            {
                buffer = null;
                this.length = 0;
                return;
            }

            IntPtr totalSize = NativeCollectionUtility.CheckedByteSize(sizeof(T), length);
            buffer = (T*)Marshal.AllocHGlobal(totalSize);
            this.length = length;

            for (int i = 0; i < length; i++)
            {
                buffer[i] = default;
            }
        }

        public ref T this[int index]
        {
            get
            {
                ThrowIfDisposed();
                if ((uint)index >= (uint)length)
                    throw new ArgumentOutOfRangeException(nameof(index));
                return ref buffer[index];
            }
        }

        public T* GetUnsafePtr()
        {
            ThrowIfDisposed();
            return buffer;
        }

        /// <summary>
        /// Borrows the backing buffer as a read-only struct view. The view
        /// captures the pointer + length once, so per-element reads skip the
        /// parent's <c>Volatile.Read</c> disposal check that
        /// <see cref="this[int]"/> performs. Intended for use inside job
        /// structs marked <see cref="Jobs.ReadOnlyAttribute"/>.
        ///
        /// <para>
        /// <b>Lifetime contract:</b> the view borrows the underlying buffer;
        /// do not use after the parent <see cref="NativeArray{T}"/> is
        /// disposed. Same contract as <see cref="GetUnsafePtr"/>.
        /// </para>
        /// </summary>
        public ReadOnly AsReadOnly()
        {
            ThrowIfDisposed();
            return new ReadOnly(buffer, length);
        }

        /// <summary>
        /// Borrows the backing buffer as a write-only struct view. The view
        /// captures the pointer + length once and exposes a set-only indexer
        /// (<see cref="WriteOnly.this[int]"/>) so accidental reads do not
        /// compile. Intended for use inside job structs marked
        /// <see cref="Jobs.WriteOnlyAttribute"/>.
        ///
        /// <para>
        /// <b>Lifetime contract:</b> see <see cref="AsReadOnly"/>.
        /// </para>
        /// </summary>
        public WriteOnly AsWriteOnly()
        {
            ThrowIfDisposed();
            return new WriteOnly(buffer, length);
        }

        public void CopyFrom(T[] source)
        {
            ThrowIfDisposed();
            if (source == null)
                throw new ArgumentNullException(nameof(source));
            if (source.Length != length)
                throw new ArgumentException("Source length must match NativeArray length.", nameof(source));
            if (length == 0)
                return;

            fixed (T* srcPtr = source)
            {
                long bytes = (long)sizeof(T) * length;
                Buffer.MemoryCopy(srcPtr, buffer, bytes, bytes);
            }
        }
        public void CopyTo(T[] destination)
        {
            ThrowIfDisposed();
            if (destination == null)
                throw new ArgumentNullException(nameof(destination));
            if (destination.Length != length)
                throw new ArgumentException("Destination length must match NativeArray length.", nameof(destination));
            if (length == 0)
                return;

            fixed (T* dstPtr = destination)
            {
                long bytes = (long)sizeof(T) * length;
                Buffer.MemoryCopy(buffer, dstPtr, bytes, bytes);
            }
        }

        public void Fill(in T value)
        {
            ThrowIfDisposed();
            if (length == 0)
                return;
            if (buffer == null)
                throw new InvalidOperationException("NativeArray is not created.");

            buffer[0] = value;

            int filled = 1;
            int sizeOfT = sizeof(T);

            while (filled < length)
            {
                int toCopy = Math.Min(filled, length - filled);
                long bytesToCopy = (long)sizeOfT * toCopy;
                long destSize = (long)sizeOfT * (length - filled);

                Buffer.MemoryCopy(
                    buffer,
                    buffer + filled,
                    destSize,
                    bytesToCopy);

                filled += toCopy;
            }
        }

        public Enumerator GetEnumerator()
        {
            ThrowIfDisposed();
            return new Enumerator(this);
        }

        public struct Enumerator
        {
            private readonly NativeArray<T> _owner;
            private readonly int _capturedVersion;
            private int _index;

            internal Enumerator(NativeArray<T> array)
            {
                _owner = array;
                _capturedVersion = array.version;
                _index = -1;
            }

            public T Current
            {
                get
                {
                    ValidateVersion();
                    if ((uint)_index >= (uint)_owner.length)
                        throw new InvalidOperationException("Enumeration has not started or has already finished.");
                    return _owner.buffer[_index];
                }
            }

            public bool MoveNext()
            {
                ValidateVersion();
                _index++;
                return _index < _owner.length;
            }

            private void ValidateVersion()
            {
                _owner.ThrowIfDisposed();
                if (_capturedVersion != _owner.version)
                    throw new InvalidOperationException("Collection was modified during enumeration.");
            }
        }

        /// <summary>
        /// Zero-overhead read-only view of a <see cref="NativeArray{T}"/>.
        /// Holds a raw <c>T* + length</c>; per-element reads skip the
        /// parent's disposal check. Obtain via
        /// <see cref="NativeArray{T}.AsReadOnly"/>.
        /// </summary>
        public readonly struct ReadOnly
        {
            private readonly T* m_Buffer;
            private readonly int m_Length;

            internal ReadOnly(T* buffer, int length)
            {
                m_Buffer = buffer;
                m_Length = length;
            }

            public int Length => m_Length;

            public T this[int index]
            {
                get
                {
                    if ((uint)index >= (uint)m_Length)
                        throw new ArgumentOutOfRangeException(nameof(index));
                    return m_Buffer[index];
                }
            }

            public T* GetUnsafeReadOnlyPtr() => m_Buffer;
        }

        /// <summary>
        /// Write-only view of a <see cref="NativeArray{T}"/>. Exposes a
        /// set-only indexer so accidental reads do not compile; per-element
        /// writes skip the parent's disposal check. Obtain via
        /// <see cref="NativeArray{T}.AsWriteOnly"/>.
        /// </summary>
        public readonly struct WriteOnly
        {
            private readonly T* m_Buffer;
            private readonly int m_Length;

            internal WriteOnly(T* buffer, int length)
            {
                m_Buffer = buffer;
                m_Length = length;
            }

            public int Length => m_Length;

            public T this[int index]
            {
                set
                {
                    if ((uint)index >= (uint)m_Length)
                        throw new ArgumentOutOfRangeException(nameof(index));
                    m_Buffer[index] = value;
                }
            }

            public T* GetUnsafeWritePtr() => m_Buffer;
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        private void Dispose(bool disposing)
        {
            if (Interlocked.Exchange(ref disposeState, 1) != 0)
                return;

            if (buffer != null)
            {
                Marshal.FreeHGlobal((IntPtr)buffer);
                buffer = null;
            }

            length = 0;
            version++;
        }

        private void ThrowIfDisposed()
        {
            if (Volatile.Read(ref disposeState) != 0)
                throw new ObjectDisposedException(nameof(NativeArray<T>));
        }
    }
}
