// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library;

import 'dart:async' show Future;
import 'dart:typed_data';
export 'dart:io' show Platform;

import 'package:compiler/compiler_api.dart' as api;

import 'package:compiler/src/io/source_file.dart'
    show Binary, StringSourceFile, Utf8BytesSourceFile;

import 'package:compiler/src/source_file_provider.dart'
    show CompilerSourceFileProvider;

export 'package:compiler/src/source_file_provider.dart'
    show SourceFileProvider, FormattingDiagnosticHandler;

class MemorySourceFileProvider extends CompilerSourceFileProvider {
  Map<String, dynamic> memorySourceFiles;

  /// MemorySourceFiles can contain maps of file names to string contents or
  /// file names to binary contents.
  MemorySourceFileProvider(this.memorySourceFiles);

  @override
  Future<api.Input<Uint8List>> readBytesFromUri(
    Uri resourceUri,
    api.InputKind inputKind,
  ) {
    if (!resourceUri.isScheme('memory')) {
      return super.readBytesFromUri(resourceUri, inputKind);
    }
    // TODO(johnniwinther): We should use inputs already in the cache. Some
    // tests currently require that we always create a fresh input.

    var source = memorySourceFiles[resourceUri.path];
    if (source == null) {
      return Future.error(
        Exception(
          'No such memory file $resourceUri in ${memorySourceFiles.keys}',
        ),
      );
    }
    api.Input<Uint8List> input;
    StringSourceFile? stringFile;
    registerUri(resourceUri);
    if (source is String) {
      stringFile = StringSourceFile.fromUri(resourceUri, source);
    }
    switch (inputKind) {
      case api.InputKind.utf8:
        input = stringFile ?? Utf8BytesSourceFile(resourceUri, source);
        break;
      case api.InputKind.binary:
        if (stringFile != null) {
          source = stringFile.data;
        }
        input = Binary(resourceUri, source);
        break;
    }
    return Future.value(input);
  }

  @override
  Future<api.Input<Uint8List>> readFromUri(
    Uri uri, {
    api.InputKind inputKind = api.InputKind.utf8,
  }) => readBytesFromUri(uri, inputKind);

  @override
  api.Input<Uint8List>? getUtf8SourceFile(Uri resourceUri) {
    var source = memorySourceFiles[resourceUri.path];
    if (source == null) return null;
    return source is String
        ? StringSourceFile.fromUri(resourceUri, source)
        : Utf8BytesSourceFile(resourceUri, source);
  }
}
