/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

package com.google.zetasql;

import com.google.common.collect.ImmutableList;
import com.google.protobuf.DescriptorProtos.FileDescriptorSet;
import com.google.protobuf.Descriptors.FileDescriptor;
import com.google.zetasql.LocalService.AnalyzeRequest;
import com.google.zetasql.LocalService.AnalyzeResponse;
import com.google.zetasql.LocalService.BuildSqlRequest;
import com.google.zetasql.LocalService.DescriptorPoolListProto;
import com.google.zetasql.functions.ZetaSQLDateTime.DateTimestampPart;
import com.google.zetasql.resolvedast.DeserializationHelper;
import com.google.zetasql.resolvedast.ResolvedNodes.ResolvedExpr;
import com.google.zetasql.resolvedast.ResolvedNodes.ResolvedStatement;

class AnalyzerHelper {
  static {
    ZetaSQLDescriptorPool.importIntoGeneratedPool(DateTimestampPart.getDescriptor());
  }

  private static DescriptorPoolListProto createDescriptorPoolList(
      FileDescriptorSetsBuilder fileDescriptorSetsBuilder) {
    DescriptorPoolListProto.Builder poolListProto = DescriptorPoolListProto.newBuilder();
    for (DescriptorPool pool : fileDescriptorSetsBuilder.getDescriptorPools()) {
      if (pool == BuiltinDescriptorPool.getInstance()) {
        poolListProto.addDefinitionsBuilder().getBuiltinBuilder();
      } else {
        FileDescriptorSet.Builder fileDescriptorSetBuilder =
            poolListProto.addDefinitionsBuilder().getFileDescriptorSetBuilder();
        for (FileDescriptor file : pool.getAllFileDescriptorsInDependencyOrder()) {
          fileDescriptorSetBuilder.addFile(file.toProto());
        }
      }
    }
    return poolListProto.build();
  }

  public static FileDescriptorSetsBuilder serializeSimpleCatalog(
      SimpleCatalog catalog, AnalyzerOptions options, AnalyzeRequest.Builder request) {
    FileDescriptorSetsBuilder fileDescriptorSetsBuilder;
    if (catalog.isRegistered()) {
      fileDescriptorSetsBuilder = catalog.getRegisteredFileDescriptorSetsBuilder();
      request.setRegisteredCatalogId(catalog.getRegisteredId());
      // If the options contains a parameter/column with a ProtoType/EnumType
      // not in the catalog, the resulting proto will contain incomplete Types,
      // which will result in deserialization failure at server side and cause
      // SqlException.
      // TODO: Check for such situation and throw a more descriptive
      // exception, or fallback to use the full serialized catalog as if it is
      // unregistered.
      request.setOptions(options.serialize(fileDescriptorSetsBuilder));
    } else {
      fileDescriptorSetsBuilder = new FileDescriptorSetsBuilder();
      fileDescriptorSetsBuilder.addAllFileDescriptors(BuiltinDescriptorPool.getInstance());
      request.setSimpleCatalog(catalog.serialize(fileDescriptorSetsBuilder));
      request.setOptions(options.serialize(fileDescriptorSetsBuilder));
      request.setDescriptorPoolList(createDescriptorPoolList(fileDescriptorSetsBuilder));
    }
    return fileDescriptorSetsBuilder;
  }

  /**
   * Includes a hack to allow DatePart enums to be properly serialized, however this doesn't support
   * registered catalogs.
   */
  public static FileDescriptorSetsBuilder serializeSimpleCatalog(
      SimpleCatalog catalog, BuildSqlRequest.Builder request) {
    FileDescriptorSetsBuilder fileDescriptorSetsBuilder;
    if (catalog.isRegistered()) {
      fileDescriptorSetsBuilder = catalog.getRegisteredFileDescriptorSetsBuilder();
      request.setRegisteredCatalogId(catalog.getRegisteredId());
    } else {
      fileDescriptorSetsBuilder = new FileDescriptorSetsBuilder();
      fileDescriptorSetsBuilder.addAllFileDescriptors(BuiltinDescriptorPool.getInstance());

      request.setSimpleCatalog(catalog.serialize(fileDescriptorSetsBuilder));
      request.setDescriptorPoolList(createDescriptorPoolList(fileDescriptorSetsBuilder));
    }
    return fileDescriptorSetsBuilder;
  }

  public static ResolvedStatement deserializeResolvedStatement(
      SimpleCatalog catalog,
      FileDescriptorSetsBuilder fileDescriptorSetsBuilder,
      AnalyzeResponse response) {
    DeserializationHelper deserializationHelper =
        getDesializationHelper(catalog, fileDescriptorSetsBuilder);
    return ResolvedStatement.deserialize(response.getResolvedStatement(), deserializationHelper);
  }

  public static ResolvedExpr deserializeResolvedExpression(
      SimpleCatalog catalog,
      FileDescriptorSetsBuilder fileDescriptorSetsBuilder,
      AnalyzeResponse response) {
    DeserializationHelper deserializationHelper =
        getDesializationHelper(catalog, fileDescriptorSetsBuilder);
    return ResolvedExpr.deserialize(response.getResolvedExpression(), deserializationHelper);
  }

  public static DeserializationHelper getDesializationHelper(
      SimpleCatalog catalog, FileDescriptorSetsBuilder fileDescriptorSetsBuilder) {
    ImmutableList.Builder<DescriptorPool> pools =
        ImmutableList.<DescriptorPool>builder()
            .addAll(fileDescriptorSetsBuilder.getDescriptorPools())
            // DateTimestampPart is the only non-simple type used in built-in functions.
            // Its descriptor comes from the generated descriptor pool on C++ side.
            // The C++ generated descriptor pool won't match any descriptor pools passed
            // from Java - because they get serialized to new objects, so it will result
            // in a new FileDescriptorSet index. As a result, we need to append the Java
            // generated pool with DateTimestampPart imported, to deserialize the type
            // properly.
            .add(ZetaSQLDescriptorPool.getGeneratedPool());

    return new DeserializationHelper(catalog.getTypeFactory(), pools.build(), catalog);
  }
}
