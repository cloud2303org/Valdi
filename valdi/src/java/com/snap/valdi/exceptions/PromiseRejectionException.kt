package com.snap.valdi.exceptions

import androidx.annotation.Keep

/**
 * An explicit Promise rejection whose message is already suitable for its consumer.
 * CppPromiseCallback forwards this message verbatim, including an empty string, while
 * other throwables retain their diagnostic class names and cause chains.
 *
 * [errorCode] keeps its existing native meaning; text alone does not imply cancellation.
 * Callers are responsible for any public-message fallback or normalization.
 */
@Keep
class PromiseRejectionException @JvmOverloads constructor(message: String, errorCode: Int = 0):
    ValdiException(message, errorCode)
